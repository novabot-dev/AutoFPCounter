#include "GdrReader.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../core/Constants.hpp"
#include "JsonLite.hpp"

namespace afpc::gdr {
namespace {

constexpr char kMagic[3] = {'G', 'D', 'R'};
constexpr std::size_t kMagicSize = 3;

// A varint needs at most ceil(64/7) = 10 bytes. Anything longer is corruption.
constexpr int kMaxVarintBytes = 10;

// ---------------------------------------------------------------------------
// Byte cursor over the v2 binary stream.
//
// Deliberately strict. The upstream reference reader returns zero and advances
// nothing when it runs out of bytes, which silently turns a truncated file into
// a replay full of frame 0 inputs. Every read here reports failure instead.
// ---------------------------------------------------------------------------
class Cursor {
public:
    Cursor(const std::uint8_t* data, std::size_t size) noexcept
        : m_data(data), m_size(size) {}

    [[nodiscard]] std::size_t remaining() const noexcept { return m_size - m_pos; }
    [[nodiscard]] bool empty() const noexcept { return m_pos >= m_size; }
    [[nodiscard]] std::size_t pos() const noexcept { return m_pos; }
    [[nodiscard]] const char* error() const noexcept { return m_error; }

    bool raw(char* dest, std::size_t n) noexcept {
        if (n > remaining()) return fail("truncated");
        std::memcpy(dest, m_data + m_pos, n);
        m_pos += n;
        return true;
    }

    bool skip(std::uint64_t n) noexcept {
        if (n > remaining()) return fail("truncated");
        m_pos += static_cast<std::size_t>(n);
        return true;
    }

    // LEB128: 7 payload bits per byte, bit 7 set means "another byte follows".
    bool varint(std::uint64_t& out) noexcept {
        std::uint64_t value = 0;
        int shift = 0;
        for (int i = 0; i < kMaxVarintBytes; ++i) {
            if (empty()) return fail("truncated varint");
            const std::uint8_t b = m_data[m_pos++];
            // Once shift reaches 63 only the final bit may be set, otherwise the
            // value would not fit in 64 bits.
            if (shift >= 64 || (shift == 63 && (b & 0x7f) > 1)) {
                return fail("varint overflows 64 bits");
            }
            value |= static_cast<std::uint64_t>(b & 0x7f) << shift;
            if ((b & 0x80) == 0) {
                out = value;
                return true;
            }
            shift += 7;
        }
        return fail("varint longer than 10 bytes");
    }

    bool varintTo(std::uint32_t& out) noexcept {
        std::uint64_t v = 0;
        if (!varint(v)) return false;
        if (v > 0xFFFFFFFFull) return fail("value does not fit in 32 bits");
        out = static_cast<std::uint32_t>(v);
        return true;
    }

    bool varintTo(int& out) noexcept {
        std::uint64_t v = 0;
        if (!varint(v)) return false;
        if (v > 0x7FFFFFFFull) return fail("value does not fit in a signed int");
        out = static_cast<int>(v);
        return true;
    }

    bool boolean(bool& out) noexcept {
        std::uint64_t v = 0;
        if (!varint(v)) return false;
        out = v != 0;
        return true;
    }

    // Length-prefixed, *not* NUL terminated. The upstream readme says otherwise;
    // the reader in binarystream.hpp is authoritative and prefixes the length.
    bool string(std::string& out) noexcept {
        std::uint64_t len = 0;
        if (!varint(len)) return false;
        // Each byte of the payload is one byte of buffer, so this also rejects a
        // corrupt length before any allocation.
        if (len > remaining()) return fail("string length exceeds remaining buffer");
        out.assign(reinterpret_cast<const char*>(m_data + m_pos), static_cast<std::size_t>(len));
        m_pos += static_cast<std::size_t>(len);
        return true;
    }

    // Fixed-width fields are big endian: binarystream.hpp memcpy's then reverses
    // on a little-endian host.
    bool be32(float& out) noexcept {
        std::uint8_t b[4];
        if (!raw(reinterpret_cast<char*>(b), 4)) return false;
        const std::uint32_t bits = (static_cast<std::uint32_t>(b[0]) << 24) |
                                   (static_cast<std::uint32_t>(b[1]) << 16) |
                                   (static_cast<std::uint32_t>(b[2]) << 8) |
                                   static_cast<std::uint32_t>(b[3]);
        std::memcpy(&out, &bits, sizeof(out));
        return true;
    }

    bool be64(double& out) noexcept {
        std::uint8_t b[8];
        if (!raw(reinterpret_cast<char*>(b), 8)) return false;
        std::uint64_t bits = 0;
        for (int i = 0; i < 8; ++i) bits = (bits << 8) | b[i];
        std::memcpy(&out, &bits, sizeof(out));
        return true;
    }

private:
    bool fail(const char* what) noexcept {
        if (m_error == nullptr) m_error = what;
        return false;
    }

    const std::uint8_t* m_data;
    std::size_t m_size;
    std::size_t m_pos = 0;
    const char* m_error = nullptr;
};

// ---------------------------------------------------------------------------
// v2 binary
//
//   "GDR"                 3 raw bytes
//   version               varint
//   inputTag              string   (empty => no per-input extension)
//   author                string
//   description           string
//   duration              f32 big endian, seconds
//   gameVersion           varint
//   framerate             f64 big endian, ticks per second
//   seed                  varint
//   coins                 varint
//   ldm                   varint bool
//   platformer            varint bool
//   bot.name              string
//   bot.version           varint
//   level.id              varint
//   level.name            string
//   extensionSize         varint, then that many opaque bytes
//   deathCount            varint, then that many accumulating deltas
//   inputCount            varint
//   p1InputCount          varint
//   ...inputs until EOF, each:
//       packed            varint
//       [per-input extension: varint size + opaque bytes, when inputTag set]
//
// Input packing differs by mode. Platformer mode stores a 2-bit button:
//     packed = delta << 3 | button << 1 | down
// Classic mode omits the button:
//     packed = delta << 1 | down
//
// The 2-player split is positional, not per record: the first p1InputCount
// records belong to player 1 and the running frame delta resets to zero at the
// boundary so player 2's frames are relative to its own start.
// ---------------------------------------------------------------------------
bool parseBinary(const std::vector<std::uint8_t>& bytes, Replay& out, Error& err,
                 std::string& message, std::size_t maxInputs) {
    Cursor cur(bytes.data(), bytes.size());

    char magic[kMagicSize];
    if (!cur.raw(magic, kMagicSize) ||
        std::memcmp(magic, kMagic, kMagicSize) != 0) {
        err = Error::BadMagic;
        message = "not a GDR v2 stream (missing 'GDR' magic)";
        return false;
    }

    // Every read below is one clause of the header. `fail` reports the cursor's
    // own reason plus how far the stream got, which is the only useful thing to
    // say about a file that stops mid-field. A lambda rather than goto, because
    // jumping forward over these declarations is a Microsoft extension and
    // warns once per crossing.
    const auto fail = [&]() {
        err = Error::Truncated;
        message = std::string("GDR v2 stream truncated at offset ") +
                  std::to_string(cur.pos()) + " (" + cur.error() + ")";
        return false;
    };

    std::string inputTag;
    float duration = 0.0f;
    double framerate = 0.0;
    int botVersion = 0;
    std::uint64_t extensionSize = 0;
    std::uint64_t deathCount = 0;
    std::uint64_t inputCount = 0;
    std::uint64_t p1Inputs = 0;

    if (!cur.varintTo(out.version)) return fail();
    if (!cur.string(inputTag)) return fail();
    if (!cur.string(out.author)) return fail();
    if (!cur.string(out.description)) return fail();
    if (!cur.be32(duration)) return fail();
    if (!cur.varintTo(out.gameVersion)) return fail();
    if (!cur.be64(framerate)) return fail();
    if (!cur.varintTo(out.seed)) return fail();
    if (!cur.varintTo(out.coins)) return fail();
    if (!cur.boolean(out.ldm)) return fail();
    if (!cur.boolean(out.platformer)) return fail();
    if (!cur.string(out.botName)) return fail();
    // An int on the wire, but v1 carries it as a JSON string, so it is stored as
    // text here rather than as a number.
    if (!cur.varintTo(botVersion)) return fail();
    if (!cur.varintTo(out.levelId)) return fail();
    if (!cur.string(out.levelName)) return fail();

    out.botVersion = std::to_string(botVersion);
    // Duration is informational only; a non-finite value means a corrupt header.
    out.duration = std::isfinite(duration) ? static_cast<double>(duration) : 0.0;
    out.framerate = std::isfinite(framerate) ? framerate : 0.0;

    if (!cur.varint(extensionSize)) return fail();
    if (!cur.skip(extensionSize)) return fail();

    // Deaths are delta encoded and monotonically accumulated. Each costs at least
    // one byte, so a count beyond the buffer is corruption, not a real document.
    if (!cur.varint(deathCount)) return fail();
    if (deathCount > cur.remaining() + 1) {
        err = Error::LengthOverflow;
        message = "death count exceeds the remaining buffer";
        return false;
    }
    out.deaths.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(deathCount, 4096)));
    {
        std::uint64_t previous = 0;
        for (std::uint64_t i = 0; i < deathCount; ++i) {
            std::uint64_t delta = 0;
            if (!cur.varint(delta)) return fail();
            previous += delta;
            out.deaths.push_back(previous);
        }
    }

    if (!cur.varint(inputCount)) return fail();
    if (inputCount > maxInputs) {
        err = Error::TooManyInputs;
        message = "input count " + std::to_string(inputCount) +
                  " exceeds the safety ceiling of " + std::to_string(maxInputs);
        return false;
    }

    if (!cur.varint(p1Inputs)) return fail();

    out.inputs.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(inputCount, 4096)));

    const bool hasInputExtension = !inputTag.empty();
    std::uint64_t previousFrame = 0;

    // The reference loop reads until the stream is exhausted rather than
    // trusting inputCount, so trailing records are still decoded. inputCount is
    // used only as the allocation bound above.
    while (!cur.empty()) {
        std::uint64_t packed = 0;
        if (!cur.varint(packed)) return fail();

        Input in;
        in.frame = previousFrame + (out.platformer ? (packed >> 3) : (packed >> 1));
        in.button = out.platformer ? static_cast<std::uint8_t>((packed >> 1) & 3u) : 1u;
        in.down = (packed & 1u) != 0;
        in.player2 = (p1Inputs == 0);

        if (hasInputExtension) {
            std::uint64_t extSize = 0;
            if (!cur.varint(extSize)) return fail();
            if (!cur.skip(extSize)) return fail();
        }

        // Guard against a frame value that would overflow the importer's 32-bit
        // tick field rather than wrapping silently.
        if (in.frame > 0xFFFFFFFFull) {
            err = Error::LengthOverflow;
            message = "input frame " + std::to_string(in.frame) + " exceeds 32-bit range";
            return false;
        }

        out.inputs.push_back(in);
        previousFrame = in.frame;

        if (p1Inputs > 0) {
            --p1Inputs;
            if (p1Inputs == 0) previousFrame = 0;
        }
    }

    // The declared count must be satisfied exactly. Without this check a stream
    // truncated partway through its last record decodes "successfully" into a
    // shorter replay, because the loop above ends on EOF rather than on the
    // count. A macro that quietly loses its last few clicks is worse than one
    // that is rejected, so the count is authoritative.
    if (out.inputs.size() != inputCount) {
        err = Error::Truncated;
        message = "GDR v2 stream declares " + std::to_string(inputCount) +
                  " inputs but only " + std::to_string(out.inputs.size()) +
                  " could be decoded (truncated file?)";
        return false;
    }

    // Player 2 records follow player 1's on disk; the importer wants them
    // interleaved in frame order.
    std::stable_sort(out.inputs.begin(), out.inputs.end(),
                     [](const Input& a, const Input& b) { return a.frame < b.frame; });

    if (out.version != 2) {
        err = Error::UnsupportedVersion;
        message = "GDR version " + std::to_string(out.version) +
                  " in a binary stream is not supported (expected 2)";
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// v1
//
// The payload is a JSON document - canonically MessagePack, with plain JSON
// accepted - carrying the fields below. Both encodings decode into the same
// json::Value tree, so this is one extraction routine.
//
//   inputs[] : frame (number), btn (number), "2p" (bool), down (bool)
//   framerate: optional, defaults to 240 when absent
// ---------------------------------------------------------------------------
void copyJsonString(const json::Value* v, std::string& out) {
    if (v != nullptr && v->isString()) out = v->asString();
}

void copyJsonNumber(const json::Value* v, double& out) {
    if (v != nullptr && v->isNumber() && std::isfinite(v->asNumber())) out = v->asNumber();
}

void copyJsonInt(const json::Value* v, int& out) {
    double d = 0.0;
    copyJsonNumber(v, d);
    if (d >= -2147483648.0 && d <= 2147483647.0) out = static_cast<int>(d);
}

bool parseDocument(const json::Value& root, Replay& out, Error& err,
                   std::string& message, std::size_t maxInputs) {
    if (!root.isObject()) {
        err = Error::MalformedInputs;
        message = "GDR v1 root is not an object";
        return false;
    }

    copyJsonInt(root.find("version"), out.version);
    copyJsonString(root.find("author"), out.author);
    copyJsonString(root.find("description"), out.description);
    copyJsonInt(root.find("gameVersion"), out.gameVersion);
    copyJsonInt(root.find("seed"), out.seed);
    copyJsonInt(root.find("coins"), out.coins);
    copyJsonNumber(root.find("duration"), out.duration);
    copyJsonNumber(root.find("framerate"), out.framerate);
    if (const json::Value* ldm = root.find("ldm"); ldm != nullptr) out.ldm = ldm->asBool(false);
    if (const json::Value* plat = root.find("platformer"); plat != nullptr) {
        out.platformer = plat->asBool(false);
    }

    if (const json::Value* bot = root.find("bot"); bot != nullptr && bot->isObject()) {
        copyJsonString(bot->find("name"), out.botName);
        if (const json::Value* v = bot->find("version"); v != nullptr && v->isNumber()) {
            out.botVersion = std::to_string(static_cast<long long>(v->asNumber()));
        } else {
            copyJsonString(bot->find("version"), out.botVersion);
        }
    }
    if (const json::Value* level = root.find("level"); level != nullptr && level->isObject()) {
        double id = 0.0;
        copyJsonNumber(level->find("id"), id);
        if (id >= 0.0) out.levelId = static_cast<std::uint32_t>(std::min(id, 4294967295.0));
        copyJsonString(level->find("name"), out.levelName);
    }

    const json::Value* inputs = root.find("inputs");
    if (inputs == nullptr || !inputs->isArray()) {
        err = Error::MalformedInputs;
        message = "GDR v1 document has no 'inputs' array";
        return false;
    }
    if (inputs->size() > maxInputs) {
        err = Error::TooManyInputs;
        message = "GDR v1 input count " + std::to_string(inputs->size()) +
                  " exceeds the safety ceiling of " + std::to_string(maxInputs);
        return false;
    }

    out.inputs.reserve(inputs->size());
    for (std::size_t i = 0; i < inputs->size(); ++i) {
        const json::Value& ev = inputs->at(i);
        if (!ev.isObject()) {
            err = Error::MalformedInputs;
            message = "GDR v1 input " + std::to_string(i) + " is not an object";
            return false;
        }

        double frame = 0.0;
        copyJsonNumber(ev.find("frame"), frame);
        if (frame < 0.0 || frame > 4294967295.0) {
            err = Error::MalformedInputs;
            message = "GDR v1 input " + std::to_string(i) + " has an out-of-range frame";
            return false;
        }

        Input in;
        in.frame = static_cast<std::uint64_t>(frame);
        double btn = 1.0;
        copyJsonNumber(ev.find("btn"), btn);
        in.button = static_cast<std::uint8_t>(std::clamp(btn, 0.0, 3.0));
        // The player-2 key is literally "2p" in this schema.
        if (const json::Value* p2 = ev.find("2p"); p2 != nullptr) in.player2 = p2->asBool(false);
        if (const json::Value* down = ev.find("down"); down != nullptr) in.down = down->asBool(false);
        out.inputs.push_back(in);
    }

    std::stable_sort(out.inputs.begin(), out.inputs.end(),
                     [](const Input& a, const Input& b) { return a.frame < b.frame; });
    return true;
}

} // namespace

bool looksLikeBinary(const std::vector<std::uint8_t>& bytes) noexcept {
    return bytes.size() >= kMagicSize &&
           std::memcmp(bytes.data(), kMagic, kMagicSize) == 0;
}

bool looksLikeJsonDocument(std::string_view text) noexcept {
    std::size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            ++i;
            continue;
        }
        break;
    }
    if (i >= text.size() || text[i] != '{') return false;

    // Cheap structural probe. A full parse would be wasted work here, and the
    // real parse re-validates everything anyway. The "inputs" key is what
    // distinguishes GDR from Mega Hack's JSON, which uses "events".
    const std::size_t probe = std::min<std::size_t>(text.size(), 4096);
    const std::string_view head = text.substr(0, probe);
    return head.find("\"inputs\"") != std::string_view::npos ||
           head.find("\"bot\"") != std::string_view::npos ||
           head.find("\"gameVersion\"") != std::string_view::npos;
}

bool parse(const std::vector<std::uint8_t>& bytes, Replay& out, Error& err,
           std::string& message, std::size_t maxInputs) {
    err = Error::None;
    message.clear();

    if (bytes.empty()) {
        err = Error::Truncated;
        message = "empty buffer";
        return false;
    }

    if (looksLikeBinary(bytes)) return parseBinary(bytes, out, err, message, maxInputs);

    // v1: MessagePack is canonical, plain JSON is accepted as a fallback. The
    // two are distinguished by the leading byte: msgpack never starts with '{'.
    std::string jsonError;
    json::Value root;
    if (static_cast<char>(bytes[0]) == '{') {
        root = json::parse(std::string_view(reinterpret_cast<const char*>(bytes.data()),
                                            bytes.size()),
                           &jsonError);
    } else {
        root = json::parseMsgPack(std::string_view(reinterpret_cast<const char*>(bytes.data()),
                                                   bytes.size()),
                                  &jsonError);
    }

    if (root.isNull()) {
        err = Error::MalformedInputs;
        message = "GDR v1 document could not be decoded: " + jsonError;
        return false;
    }

    if (!parseDocument(root, out, err, message, maxInputs)) return false;

    if (out.version != 1) {
        err = Error::UnsupportedVersion;
        message = "GDR version " + std::to_string(out.version) +
                  " in a v1 document is not supported (expected 1)";
        return false;
    }
    return true;
}

} // namespace afpc::gdr
