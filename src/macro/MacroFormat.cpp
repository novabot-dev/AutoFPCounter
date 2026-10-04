#include "MacroFormat.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string_view>

#include "../core/Constants.hpp"
#include "JsonLite.hpp"

namespace afpc {
namespace {

// ---------------------------------------------------------------------------
// Small strict scalar parsers.
//
// Every one of these requires the *entire* token to be consumed. strtod /
// strtoul happily stop at the first bad character, which is how "12abc" turns
// into 12 and silently shifts every subsequent field of a macro row.
// ---------------------------------------------------------------------------

bool parseDoubleStrict(std::string_view token, double& out) noexcept {
    while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) token.remove_prefix(1);
    while (!token.empty() && (token.back() == ' ' || token.back() == '\t' || token.back() == '\r')) {
        token.remove_suffix(1);
    }
    if (token.empty()) return false;

    const std::string copy(token);
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(copy.c_str(), &end);
    if (end != copy.c_str() + copy.size()) return false;
    if (!std::isfinite(value)) return false;
    out = value;
    return true;
}

bool parseUintStrict(std::string_view token, std::uint32_t& out) noexcept {
    while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) token.remove_prefix(1);
    while (!token.empty() && (token.back() == ' ' || token.back() == '\t' || token.back() == '\r')) {
        token.remove_suffix(1);
    }
    if (token.empty()) return false;

    for (char c : token) {
        if (c < '0' || c > '9') return false;
    }

    const std::string copy(token);
    char* end = nullptr;
    errno = 0;
    const unsigned long long value = std::strtoull(copy.c_str(), &end, 10);
    if (end != copy.c_str() + copy.size()) return false;
    if (errno == ERANGE) return false;
    if (value > 0xFFFFFFFFull) return false;
    out = static_cast<std::uint32_t>(value);
    return true;
}

bool isBlankOrComment(std::string_view line) noexcept {
    for (char c : line) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        return c == '#';
    }
    return true;
}

std::vector<std::string_view> collectLines(std::string_view text) {
    std::vector<std::string_view> lines;
    lines.reserve(64);

    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();

        std::string_view line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

        if (!isBlankOrComment(line)) lines.push_back(line);

        if (end == text.size()) break;
        start = end + 1;
    }
    return lines;
}

std::vector<std::string_view> split(std::string_view line, char delim) {
    std::vector<std::string_view> parts;
    parts.reserve(8);
    std::size_t start = 0;
    for (;;) {
        std::size_t end = line.find(delim, start);
        if (end == std::string_view::npos) {
            parts.push_back(line.substr(start));
            break;
        }
        parts.push_back(line.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

std::vector<std::string_view> splitWhitespace(std::string_view line) {
    std::vector<std::string_view> parts;
    parts.reserve(6);

    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i;
        if (i >= line.size()) break;
        const std::size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') ++i;
        parts.push_back(line.substr(start, i - start));
    }
    return parts;
}

bool contains(std::string_view haystack, char needle) noexcept {
    return haystack.find(needle) != std::string_view::npos;
}

// ---- little-endian scalar readers ----------------------------------------

std::uint32_t readU32LE(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

double readF64LE(const std::uint8_t* p) noexcept {
    std::uint64_t bits = 0;
    for (int i = 7; i >= 0; --i) bits = (bits << 8) | static_cast<std::uint64_t>(p[i]);
    double value = 0.0;
    static_assert(sizeof(value) == sizeof(bits), "double must be 64 bit");
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void pushU32LE(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
}

void pushF64LE(std::vector<std::uint8_t>& out, double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>((bits >> (i * 8)) & 0xFF));
    }
}

void pushText(std::vector<std::uint8_t>& out, std::string_view text) {
    out.insert(out.end(), text.begin(), text.end());
}

// Silicate packs the frame into the top 28 bits of a u32.
constexpr std::uint32_t kSilicateFrameMask = 0x0FFFFFFFu;

// ---------------------------------------------------------------------------
// Shared post-processing
// ---------------------------------------------------------------------------

// Stable-sorts by tick and drops exact duplicate rows, then recomputes maxTick.
// std::stable_sort already does an insertion-sort pass for small or nearly
// sorted ranges, so a real (already ordered) macro costs close to nothing here.
void finaliseInputs(MacroData& data) {
    std::stable_sort(data.inputs.begin(), data.inputs.end(),
                     [](const MacroInput& a, const MacroInput& b) {
                         if (a.tick != b.tick) return a.tick < b.tick;
                         if (a.down != b.down) return a.down;
                         return !a.player2 && b.player2;
                     });

    std::size_t write = 0;
    for (std::size_t read = 0; read < data.inputs.size(); ++read) {
        const MacroInput& in = data.inputs[read];
        if (in.tick > kSilicateFrameMask) {
            data.diagnostics += "dropped input with out-of-range tick " +
                                std::to_string(in.tick) + "; ";
            continue;
        }
        if (write > 0) {
            const MacroInput& prev = data.inputs[write - 1];
            if (prev.tick == in.tick && prev.down == in.down && prev.player2 == in.player2) {
                continue; // exact duplicate row
            }
        }
        data.inputs[write++] = in;
        if (in.tick > data.maxTick) data.maxTick = in.tick;
    }
    data.inputs.resize(write);
}

std::string bytesToString(const std::vector<std::uint8_t>& bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

bool startsWith(const std::vector<std::uint8_t>& bytes, std::string_view prefix) {
    if (bytes.size() < prefix.size()) return false;
    return std::memcmp(bytes.data(), prefix.data(), prefix.size()) == 0;
}

bool looksLikeSilicateBinary(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 12) return false;

    const double fps = readF64LE(bytes.data());
    if (!std::isfinite(fps)) return false;
    if (fps < kMinPlausibleTps || fps > kMaxPlausibleTps) return false;

    const std::uint32_t count = readU32LE(bytes.data() + 8);
    if (static_cast<std::size_t>(count) > kMaxMacroInputs) return false;

    const std::size_t payload = bytes.size() - 12;
    const std::size_t expected = static_cast<std::size_t>(count) * 4u;
    if (expected > payload) return false;
    // Allow up to 3 trailing bytes (some writers pad); anything more means the
    // f64/u32 header match was a coincidence.
    return (payload - expected) <= 3u;
}

std::string formatFpsForHeader(double fps) {
    if (std::fabs(fps - std::floor(fps + 0.5)) < 1e-9) {
        return std::to_string(static_cast<long long>(std::floor(fps + 0.5)));
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.6f", fps);
    return std::string(buffer);
}

std::string jsonEscape(std::string_view in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    out += buffer;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Public format helpers
// ---------------------------------------------------------------------------

const char* formatName(MacroFormat fmt) noexcept {
    switch (fmt) {
        case MacroFormat::Silicate: return "Silicate";
        case MacroFormat::XDBot: return "XDBot";
        case MacroFormat::XBot: return "xBot";
        case MacroFormat::MegaHackJson: return "Mega Hack";
        case MacroFormat::Eclipse: return "Eclipse";
        case MacroFormat::PlainText: return "Plain Text";
        case MacroFormat::Unknown: break;
    }
    return "Unknown";
}

bool formatIsFrameBased(MacroFormat fmt) noexcept {
    // Silicate, XDBot, Mega Hack JSON, Eclipse and plain text all store an
    // explicit frame/tick index. 2.1's xBot "pro_plus" variant stores an X
    // position instead, which cannot be validated against a tick rate.
    return fmt != MacroFormat::XBot;
}

const char* rejectReasonName(RejectReason r) noexcept {
    switch (r) {
        case RejectReason::None: return "None";
        case RejectReason::ParseFailed: return "ParseFailed";
        case RejectReason::FpsNotFinite: return "FpsNotFinite";
        case RejectReason::FpsMissing: return "FpsMissing";
        case RejectReason::FpsMismatch: return "FpsMismatch";
        case RejectReason::NotFrameBased: return "NotFrameBased";
        case RejectReason::TooManyInputs: return "TooManyInputs";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// Silicate (binary)
//
//   offset 0  : f64 little endian, tick rate
//   offset 8  : u32 little endian, input count
//   offset 12 : count * u32 little endian
//
// Each u32 is a packed input:
//   bits  0      down (1 = pressed, 0 = released)
//   bits  1..2   button id, where 1 == jump; anything else is skipped
//   bit   3      player 2
//   bits 4..31   absolute frame index
// ---------------------------------------------------------------------------

MacroParseResult parseAsSilicate(const std::vector<std::uint8_t>& bytes) {
    MacroParseResult result;
    MacroData& data = result.data;

    if (bytes.size() < 12) {
        return MacroParseResult::failure(MacroError::Malformed,
                                         "Silicate file is shorter than its 12 byte header");
    }

    const double fps = readF64LE(bytes.data());
    if (!std::isfinite(fps)) {
        return MacroParseResult::failure(MacroError::NonFiniteHeader,
                                         "Silicate tick rate is NaN or infinite");
    }

    const std::uint32_t count = readU32LE(bytes.data() + 8);
    if (static_cast<std::size_t>(count) > kMaxMacroInputs) {
        return MacroParseResult::failure(MacroError::Malformed,
                                         "Silicate input count " + std::to_string(count) +
                                             " exceeds the safety ceiling");
    }

    const std::size_t available = (bytes.size() - 12) / 4u;
    if (static_cast<std::size_t>(count) > available) {
        return MacroParseResult::failure(MacroError::Malformed,
                                         "Silicate file declares " + std::to_string(count) +
                                             " inputs but only holds " + std::to_string(available));
    }

    data.format = MacroFormat::Silicate;
    data.declaredFps = fps;
    data.frameBased = true;
    data.inputs.reserve(count);

    std::uint32_t skipped = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t* p = bytes.data() + 12 + static_cast<std::size_t>(i) * 4u;
        const std::uint32_t state = readU32LE(p);

        const std::uint32_t button = (state & 0x6u) >> 1;
        if (button != 1u) {
            ++skipped;
            continue;
        }

        MacroInput input;
        input.tick = state >> 4;
        input.down = (state & 0x1u) != 0;
        input.player2 = (state & 0x8u) != 0;
        data.inputs.push_back(input);
    }

    if (skipped > 0) {
        data.diagnostics += "skipped " + std::to_string(skipped) +
                            " rows with a non-jump button id; ";
    }

    finaliseInputs(data);
    result.ok = true;
    return result;
}

// ---------------------------------------------------------------------------
// XDBot (text)
//
//   line 0 : "<fps>"
//   line N : "<frame>|<hold>|<button>|<p2>"
//
// Note on the player-2 flag: nat-converter's reader derives it with
// `field != "1"`, which inverts the flag relative to its own writer. That is a
// bug in that reader, not part of the format, so this implementation uses the
// documented mapping (`"1"` means player 2) which matches XDBot's writer.
// ---------------------------------------------------------------------------

MacroParseResult parseAsXDBot(const std::string& text) {
    MacroParseResult result;
    MacroData& data = result.data;

    const auto lines = collectLines(text);
    if (lines.empty()) {
        return MacroParseResult::failure(MacroError::Malformed, "XDBot file is empty");
    }

    double fps = 0.0;
    if (!parseDoubleStrict(lines[0], fps)) {
        return MacroParseResult::failure(MacroError::NonFiniteHeader,
                                         "XDBot header line is not a finite number");
    }

    data.format = MacroFormat::XDBot;
    data.declaredFps = fps;
    data.frameBased = true;
    data.inputs.reserve(lines.size() > 1 ? lines.size() - 1 : 0);

    std::uint32_t skipped = 0;
    for (std::size_t i = 1; i < lines.size(); ++i) {
        const auto parts = split(lines[i], '|');
        if (parts.size() < 3) {
            return MacroParseResult::failure(MacroError::Malformed,
                                             "XDBot row " + std::to_string(i) +
                                                 " has fewer than 3 pipe separated fields");
        }

        std::uint32_t frame = 0;
        if (!parseUintStrict(parts[0], frame)) {
            return MacroParseResult::failure(MacroError::Malformed,
                                             "XDBot row " + std::to_string(i) +
                                                 " has an unparsable frame field");
        }

        if (parts[2] != "1") {
            ++skipped;
            continue; // not a jump button
        }

        MacroInput input;
        input.tick = frame;
        input.down = (parts[1] == "1");
        input.player2 = parts.size() >= 4 && parts[3] == "1";
        data.inputs.push_back(input);
    }

    if (skipped > 0) {
        data.diagnostics += "skipped " + std::to_string(skipped) + " non-jump rows; ";
    }

    finaliseInputs(data);
    result.ok = true;
    return result;
}

// ---------------------------------------------------------------------------
// xBot (2.1 legacy text)
//
//   line 0 : "fps: <n>"
//   line 1 : "frames" | "pro_plus"
//   line N : "<state> <frame>"
//
// `pro_plus` files store an X position instead of a frame index. Those cannot
// be checked against a tick rate at all, so they are rejected rather than
// silently reinterpreted.
// ---------------------------------------------------------------------------

MacroParseResult parseAsXBot(const std::string& text) {
    MacroParseResult result;
    MacroData& data = result.data;

    const auto lines = collectLines(text);
    if (lines.size() < 2) {
        return MacroParseResult::failure(MacroError::Malformed, "xBot file is truncated");
    }

    constexpr std::string_view prefix = "fps:";
    if (lines[0].size() <= prefix.size() || lines[0].substr(0, prefix.size()) != prefix) {
        return MacroParseResult::failure(MacroError::Malformed,
                                         "xBot file does not start with a 'fps:' header");
    }

    double fps = 0.0;
    if (!parseDoubleStrict(lines[0].substr(prefix.size()), fps)) {
        return MacroParseResult::failure(MacroError::NonFiniteHeader,
                                         "xBot header tick rate is not a finite number");
    }

    const std::string_view mode = lines[1];
    if (mode == "pro_plus" || mode == "pro plus") {
        return MacroParseResult::failure(
            MacroError::Malformed,
            "xBot 'pro_plus' files store X positions, not frame indices; they cannot be "
            "validated against a 240 FPS tick rate and are not importable");
    }
    if (mode != "frames") {
        return MacroParseResult::failure(MacroError::Malformed,
                                         "xBot second header line must be 'frames'");
    }

    data.format = MacroFormat::XBot;
    data.declaredFps = fps;
    data.frameBased = true;
    data.inputs.reserve(lines.size() - 2);

    for (std::size_t i = 2; i < lines.size(); ++i) {
        const auto parts = splitWhitespace(lines[i]);
        if (parts.size() < 2) {
            return MacroParseResult::failure(MacroError::Malformed,
                                             "xBot row " + std::to_string(i) +
                                                 " needs a state and a frame field");
        }

        std::uint32_t state = 0;
        if (!parseUintStrict(parts[0], state)) {
            return MacroParseResult::failure(MacroError::Malformed,
                                             "xBot row " + std::to_string(i) +
                                                 " has an unparsable state field");
        }

        std::uint32_t frame = 0;
        if (!parseUintStrict(parts[1], frame)) {
            return MacroParseResult::failure(MacroError::Malformed,
                                             "xBot row " + std::to_string(i) +
                                                 " has an unparsable frame field");
        }

        MacroInput input;
        input.tick = frame;
        input.down = (state % 2u) == 1u;
        input.player2 = state > 1u;
        data.inputs.push_back(input);
    }

    finaliseInputs(data);
    result.ok = true;
    return result;
}

// ---------------------------------------------------------------------------
// Mega Hack replay (JSON)
//
//   {
//     "meta":   { "fps": 240 },
//     "events": [ { "frame": 12, "down": true, "p2": false,
//                   "x": .., "y": .., "a": .., "r": .. }, ... ]
//   }
//
// `x`, `y`, `a` and `r` are physics-correction fields. They are ignored here
// because this mod drives input, not the physics state; accepting and dropping
// them keeps the importer compatible with real Mega Hack exports.
// ---------------------------------------------------------------------------

MacroParseResult parseAsMegaHackJson(const std::string& text) {
    MacroParseResult result;
    MacroData& data = result.data;

    std::string error;
    const json::Value root = json::parse(text, &error);
    if (root.isNull()) {
        return MacroParseResult::failure(MacroError::Malformed,
                                         "Mega Hack JSON parse failed: " + error);
    }

    data.format = MacroFormat::MegaHackJson;
    data.frameBased = true;

    // ---- tick rate ------------------------------------------------------
    const json::Value* meta = root.isObject() ? root.find("meta") : nullptr;
    if (meta != nullptr && meta->isObject()) {
        if (const json::Value* fps = meta->find("fps"); fps != nullptr && fps->isNumber()) {
            data.declaredFps = fps->asNumber();
        }
    }
    if (!std::isfinite(data.declaredFps) || data.declaredFps <= 0.0) {
        // Leave declaredFps at 0 so the 240 gate reports FpsMissing with a
        // message that names the actual problem.
        data.declaredFps = 0.0;
    }

    // ---- events ---------------------------------------------------------
    const json::Value* events = nullptr;
    if (root.isArray()) {
        events = &root; // bare array of events
    } else if (root.isObject()) {
        events = root.find("events");
        if (events == nullptr) events = root.find("inputs");
    }
    if (events == nullptr || !events->isArray()) {
        return MacroParseResult::failure(MacroError::Malformed,
                                         "Mega Hack JSON has no 'events' array");
    }
    if (events->size() > kMaxMacroInputs) {
        return MacroParseResult::failure(MacroError::Malformed,
                                         "Mega Hack event count exceeds the safety ceiling");
    }

    data.inputs.reserve(events->size());

    for (std::size_t i = 0; i < events->size(); ++i) {
        const json::Value& ev = events->at(i);
        if (!ev.isObject()) {
            return MacroParseResult::failure(MacroError::Malformed,
                                             "Mega Hack event " + std::to_string(i) + " is not an object");
        }

        const json::Value* frame = ev.find("frame");
        if (frame == nullptr || !frame->isNumber()) {
            return MacroParseResult::failure(
                MacroError::Malformed,
                "Mega Hack event " + std::to_string(i) + " has no numeric 'frame'");
        }

        const double frameValue = frame->asNumber();
        if (!std::isfinite(frameValue) || frameValue < 0.0) {
            return MacroParseResult::failure(
                MacroError::NonFiniteHeader,
                "Mega Hack event " + std::to_string(i) + " has an out-of-range frame value");
        }

        MacroInput input;
        input.tick = static_cast<std::uint32_t>(
            std::min(frameValue, static_cast<double>(kSilicateFrameMask)));

        if (const json::Value* down = ev.find("down"); down != nullptr) {
            input.down = down->asBool(false);
        }
        if (const json::Value* p2 = ev.find("p2"); p2 != nullptr) {
            input.player2 = p2->asBool(false);
        } else if (const json::Value* player2 = ev.find("player2"); player2 != nullptr) {
            input.player2 = player2->asBool(false);
        }

        data.inputs.push_back(input);
    }

    finaliseInputs(data);
    result.ok = true;
    return result;
}

// ---------------------------------------------------------------------------
// Eclipse (text)
//
//   line 0 : "<fps>"
//   line 1 : "<click count>"          (optional in some exports)
//   line N : "<x>, <y>, <delay>, <down>"
//
// `delay` is a frame delta, not an absolute index, so rows are accumulated
// into absolute ticks here. X and Y are only meaningful for X-position driven
// playback and are read but discarded.
// ---------------------------------------------------------------------------

MacroParseResult parseAsEclipse(const std::string& text) {
    MacroParseResult result;
    MacroData& data = result.data;

    const auto lines = collectLines(text);
    if (lines.empty()) {
        return MacroParseResult::failure(MacroError::Malformed, "Eclipse file is empty");
    }

    double fps = 0.0;
    if (!parseDoubleStrict(lines[0], fps)) {
        return MacroParseResult::failure(MacroError::NonFiniteHeader,
                                         "Eclipse header line is not a finite number");
    }

    data.format = MacroFormat::Eclipse;
    data.declaredFps = fps;
    data.frameBased = true;

    std::size_t firstRow = 1;
    std::uint32_t declaredCount = 0;
    bool haveCount = false;

    // A count line is a bare integer with no comma. Anything else is a row.
    if (lines.size() >= 3 && !contains(lines[1], ',')) {
        if (parseUintStrict(lines[1], declaredCount)) {
            haveCount = true;
            firstRow = 2;
        }
    }

    if (lines.size() <= firstRow) {
        return MacroParseResult::failure(MacroError::Malformed,
                                         "Eclipse file has a header but no input rows");
    }

    const std::size_t availableRows = lines.size() - firstRow;
    std::size_t rows = availableRows;
    if (haveCount) {
        if (static_cast<std::size_t>(declaredCount) > availableRows) {
            data.diagnostics += "header declares " + std::to_string(declaredCount) +
                                " clicks but only " + std::to_string(availableRows) +
                                " rows are present; using the rows that exist; ";
        } else {
            rows = declaredCount;
        }
    }

    data.inputs.reserve(rows);

    std::uint32_t tick = 0;
    for (std::size_t i = 0; i < rows; ++i) {
        const auto parts = split(lines[firstRow + i], ',');
        if (parts.size() < 2) {
            return MacroParseResult::failure(MacroError::Malformed,
                                             "Eclipse row " + std::to_string(i) +
                                                 " has fewer than 2 comma separated fields");
        }

        // Accepted shapes: "x,y,delay,down" (4), "y,delay,down" (3),
        // "delay,down" (2). The delay field is always second from the right.
        std::size_t delayIndex = 0;
        std::size_t downIndex = 1;
        if (parts.size() >= 4) {
            delayIndex = parts.size() - 2;
            downIndex = parts.size() - 1;
        } else if (parts.size() == 3) {
            delayIndex = 1;
            downIndex = 2;
        }

        double delay = 0.0;
        if (!parseDoubleStrict(parts[delayIndex], delay)) {
            return MacroParseResult::failure(MacroError::Malformed,
                                             "Eclipse row " + std::to_string(i) +
                                                 " has an unparsable delay field");
        }
        if (delay < 0.0) delay = 0.0;
        if (delay > static_cast<double>(kSilicateFrameMask)) delay = static_cast<double>(kSilicateFrameMask);

        double downValue = 0.0;
        if (!parseDoubleStrict(parts[downIndex], downValue)) {
            return MacroParseResult::failure(MacroError::Malformed,
                                             "Eclipse row " + std::to_string(i) +
                                                 " has an unparsable down field");
        }

        tick += static_cast<std::uint32_t>(delay + 0.5);

        MacroInput input;
        input.tick = tick;
        input.down = downValue != 0.0;
        input.player2 = false; // Eclipse rows are player 1 only
        data.inputs.push_back(input);
    }

    finaliseInputs(data);
    result.ok = true;
    return result;
}

// ---------------------------------------------------------------------------
// Plain text (text)
//
//   line 0 : "<fps>"
//   line N : "<frame> <hold> <p2>"
// ---------------------------------------------------------------------------

MacroParseResult parseAsPlainText(const std::string& text) {
    MacroParseResult result;
    MacroData& data = result.data;

    const auto lines = collectLines(text);
    if (lines.empty()) {
        return MacroParseResult::failure(MacroError::Malformed, "macro file is empty");
    }

    double fps = 0.0;
    if (!parseDoubleStrict(lines[0], fps)) {
        return MacroParseResult::failure(MacroError::NonFiniteHeader,
                                         "header line is not a finite number");
    }

    data.format = MacroFormat::PlainText;
    data.declaredFps = fps;
    data.frameBased = true;
    data.inputs.reserve(lines.size() - 1);

    for (std::size_t i = 1; i < lines.size(); ++i) {
        const auto parts = splitWhitespace(lines[i]);
        if (parts.size() < 2) {
            return MacroParseResult::failure(MacroError::Malformed,
                                             "row " + std::to_string(i) +
                                                 " needs a frame and a hold field");
        }

        std::uint32_t frame = 0;
        if (!parseUintStrict(parts[0], frame)) {
            return MacroParseResult::failure(MacroError::Malformed,
                                             "row " + std::to_string(i) +
                                                 " has an unparsable frame field");
        }

        MacroInput input;
        input.tick = frame;
        input.down = (parts[1] == "1");
        input.player2 = parts.size() >= 3 && parts[2] == "1";
        data.inputs.push_back(input);
    }

    finaliseInputs(data);
    result.ok = true;
    return result;
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

MacroFormat detectFormat(const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) return MacroFormat::Unknown;

    std::size_t lead = 0;
    while (lead < bytes.size() && (bytes[lead] == ' ' || bytes[lead] == '\t' ||
                                   bytes[lead] == '\n' || bytes[lead] == '\r')) {
        ++lead;
    }
    if (lead < bytes.size() && bytes[lead] == '{') return MacroFormat::MegaHackJson;

    if (startsWith(bytes, "fps: ")) return MacroFormat::XBot;
    if (looksLikeSilicateBinary(bytes)) return MacroFormat::Silicate;

    const std::string text = bytesToString(bytes);
    const auto lines = collectLines(text);
    if (lines.size() < 2) return MacroFormat::Unknown;

    // The first line must be the declared tick rate; every text dialect starts
    // that way, so this doubles as a cheap sanity check against junk.
    double fps = 0.0;
    if (!parseDoubleStrict(lines[0], fps)) return MacroFormat::Unknown;

    // A delimiter anywhere in the body identifies XDBot or Eclipse outright.
    for (std::size_t row = 1; row < lines.size(); ++row) {
        if (contains(lines[row], '|')) return MacroFormat::XDBot;
    }
    for (std::size_t row = 1; row < lines.size(); ++row) {
        if (contains(lines[row], ',')) return MacroFormat::Eclipse;
    }

    // No delimiters at all: the "frame hold [p2]" dialect, which needs at least
    // two whitespace separated tokens per row. Checking the first body row rather
    // than the second keeps single-input macros identifiable.
    if (splitWhitespace(lines[1]).size() >= 2) return MacroFormat::PlainText;

    return MacroFormat::Unknown;
}

MacroParseResult parseMacroBuffer(std::vector<std::uint8_t> bytes, std::string name) {
    if (bytes.empty()) {
        return MacroParseResult::failure(MacroError::Empty, "file is empty");
    }
    if (bytes.size() > kMaxMacroBytes) {
        return MacroParseResult::failure(MacroError::TooLarge,
                                         "file exceeds the " +
                                             std::to_string(kMaxMacroBytes / (1024 * 1024)) +
                                             " MiB ingestion ceiling");
    }

    const MacroFormat fmt = detectFormat(bytes);

    MacroParseResult result;
    switch (fmt) {
        case MacroFormat::Silicate: result = parseAsSilicate(bytes); break;
        case MacroFormat::XDBot: result = parseAsXDBot(bytesToString(bytes)); break;
        case MacroFormat::XBot: result = parseAsXBot(bytesToString(bytes)); break;
        case MacroFormat::MegaHackJson: result = parseAsMegaHackJson(bytesToString(bytes)); break;
        case MacroFormat::Eclipse: result = parseAsEclipse(bytesToString(bytes)); break;
        case MacroFormat::PlainText: result = parseAsPlainText(bytesToString(bytes)); break;
        case MacroFormat::Unknown:
            return MacroParseResult::failure(
                MacroError::UnknownFormat,
                "unrecognised layout; expected Silicate binary, XDBot pipe rows, xBot "
                "'fps:/frames', Mega Hack JSON, or Eclipse comma rows");
    }

    if (result.ok) {
        result.data.sourceName = std::move(name);
        // Release the caller's buffer eagerly; decoded inputs are the only
        // state that has to survive.
        bytes.clear();
        bytes.shrink_to_fit();
    }
    return result;
}

MacroParseResult parseMacroFile(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return MacroParseResult::failure(MacroError::Empty,
                                         "cannot stat '" + path.string() + "'");
    }
    if (size == 0) {
        return MacroParseResult::failure(MacroError::Empty, "'" + path.string() + "' is empty");
    }
    if (size > kMaxMacroBytes) {
        return MacroParseResult::failure(MacroError::TooLarge,
                                         "'" + path.string() + "' exceeds the ingestion ceiling");
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return MacroParseResult::failure(MacroError::Empty,
                                         "cannot open '" + path.string() + "' for reading");
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    if (file.gcount() != static_cast<std::streamsize>(size)) {
        return MacroParseResult::failure(MacroError::Malformed,
                                         "short read on '" + path.string() + "'");
    }

    return parseMacroBuffer(std::move(bytes), path.filename().string());
}

// ---------------------------------------------------------------------------
// Serialisation
// ---------------------------------------------------------------------------

bool writeMacro(const MacroData& data, MacroFormat fmt, std::vector<std::uint8_t>& out) {
    std::vector<std::uint8_t> buffer;
    buffer.reserve(12 + data.inputs.size() * 4);

    const double fps = std::isfinite(data.declaredFps) ? data.declaredFps : static_cast<double>(kBaseTps);

    switch (fmt) {
        case MacroFormat::Silicate: {
            pushF64LE(buffer, fps);
            pushU32LE(buffer, static_cast<std::uint32_t>(data.inputs.size()));
            for (const MacroInput& in : data.inputs) {
                std::uint32_t state = static_cast<std::uint32_t>(in.tick & kSilicateFrameMask) << 4;
                if (in.player2) state |= 0x8u;
                state |= 0x2u; // button id 1 == jump
                if (in.down) state |= 0x1u;
                pushU32LE(buffer, state);
            }
            break;
        }
        case MacroFormat::XDBot: {
            pushText(buffer, formatFpsForHeader(fps));
            buffer.push_back('\n');
            for (const MacroInput& in : data.inputs) {
                pushText(buffer, std::to_string(in.tick));
                buffer.push_back('|');
                buffer.push_back(in.down ? '1' : '0');
                buffer.push_back('|');
                buffer.push_back('1');
                buffer.push_back('|');
                buffer.push_back(in.player2 ? '1' : '0');
                buffer.push_back('\n');
            }
            break;
        }
        case MacroFormat::Eclipse: {
            pushText(buffer, formatFpsForHeader(fps));
            buffer.push_back('\n');
            pushText(buffer, std::to_string(data.inputs.size()));
            buffer.push_back('\n');
            std::uint32_t previous = 0;
            for (const MacroInput& in : data.inputs) {
                const std::uint32_t delay =
                    in.tick > previous ? in.tick - previous : 0u;
                previous = in.tick;
                pushText(buffer, "0,0,");
                pushText(buffer, std::to_string(delay));
                buffer.push_back(',');
                buffer.push_back(in.down ? '1' : '0');
                buffer.push_back('\n');
            }
            break;
        }
        case MacroFormat::PlainText: {
            pushText(buffer, formatFpsForHeader(fps));
            buffer.push_back('\n');
            for (const MacroInput& in : data.inputs) {
                pushText(buffer, std::to_string(in.tick));
                buffer.push_back(' ');
                buffer.push_back(in.down ? '1' : '0');
                buffer.push_back(' ');
                buffer.push_back(in.player2 ? '1' : '0');
                buffer.push_back('\n');
            }
            break;
        }
        case MacroFormat::MegaHackJson: {
            pushText(buffer, "{\"meta\":{\"fps\":");
            pushText(buffer, formatFpsForHeader(fps));
            pushText(buffer, "},\"events\":[");
            for (std::size_t i = 0; i < data.inputs.size(); ++i) {
                const MacroInput& in = data.inputs[i];
                if (i != 0) buffer.push_back(',');
                pushText(buffer, "{\"frame\":");
                pushText(buffer, std::to_string(in.tick));
                pushText(buffer, ",\"down\":");
                pushText(buffer, in.down ? "true" : "false");
                pushText(buffer, ",\"p2\":");
                pushText(buffer, in.player2 ? "true" : "false");
                buffer.push_back('}');
            }
            pushText(buffer, "]}");
            break;
        }
        case MacroFormat::Unknown:
        case MacroFormat::XBot:
            return false; // xBot output is not supported
    }

    out = std::move(buffer);
    return true;
}

// ---------------------------------------------------------------------------
// 240 FPS gate
// ---------------------------------------------------------------------------

ValidationResult enforceNativeFps(const MacroData& data, double requiredFps, double epsilon) {
    if (!std::isfinite(data.declaredFps)) {
        return ValidationResult::reject(RejectReason::FpsNotFinite,
                                        "declared tick rate is NaN or infinite");
    }
    if (data.declaredFps <= 0.0) {
        return ValidationResult::reject(
            RejectReason::FpsMissing,
            "file carries no usable tick rate header, so a 240 FPS recording cannot be verified");
    }
    // Both signals have to agree: the parser's own verdict for this file, and the
    // format's general capability. A future X-indexed variant that slipped past
    // the parser would still be caught here.
    if (!data.frameBased || !formatIsFrameBased(data.format)) {
        return ValidationResult::reject(
            RejectReason::NotFrameBased,
            std::string(formatName(data.format)) +
                " files of this variant are indexed by X position, not by frame, so the "
                "tick rate is not verifiable");
    }

    const double delta = std::fabs(data.declaredFps - requiredFps);
    if (!(delta <= epsilon)) {
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer),
                      "declared tick rate %.6f FPS is not %.1f FPS (delta %.6f, tolerance %.6f)",
                      data.declaredFps, requiredFps, delta, epsilon);
        return ValidationResult::reject(RejectReason::FpsMismatch, buffer);
    }

    if (data.inputs.size() > kMaxMacroInputs) {
        return ValidationResult::reject(RejectReason::TooManyInputs,
                                        "decoded input count exceeds the safety ceiling");
    }

    return ValidationResult::pass();
}

} // namespace afpc