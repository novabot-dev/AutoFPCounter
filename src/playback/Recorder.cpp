#include "Recorder.hpp"

#include <fstream>

#include <Geode/loader/Mod.hpp>

#include "../core/Constants.hpp"

namespace afpc {

namespace {

std::string lowerExtension(std::filesystem::path path) {
    std::string ext = path.extension().string();
    for (char& c : ext) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return ext;
}

} // namespace

MacroFormat Recorder::formatForExtension(const std::filesystem::path& path) noexcept {
    const std::string ext = lowerExtension(path);

    if (ext == ".slc") return MacroFormat::Silicate; // unambiguous binary container
    if (ext == ".json") return MacroFormat::MegaHackJson;
    if (ext == ".csv") return MacroFormat::Eclipse;
    if (ext == ".txt") return MacroFormat::PlainText;

    // ".macro" is genuinely ambiguous: Silicate uses it for its binary container
    // and the text dialects use it too. Guessing binary here would be the
    // dangerous direction to guess wrong, because a text macro rewritten as a
    // binary header is unreadable by everything - whereas a text file with the
    // wrong extension is still self-describing and this mod's own importer can
    // sniff it back. So default to text and let the caller override.
    return MacroFormat::XDBot;
}

void Recorder::begin() noexcept {
    m_events.clear(); // capacity retained
    m_tick = 0;
    m_recording = true;
}

void Recorder::end() noexcept {
    m_recording = false;
}

void Recorder::onTick() noexcept {
    if (m_recording) ++m_tick;
}

void Recorder::onButton(bool down, bool player2) noexcept {
    if (!m_recording) return;

    // Saturate rather than wrap: a run this long is not real, but a wrapped
    // index would silently reorder every later event in the exported macro.
    if (m_tick > 0x0FFFFFFFull) {
        m_recording = false;
        return;
    }

    MacroInput input;
    input.tick = static_cast<std::uint32_t>(m_tick);
    input.down = down;
    input.player2 = player2;

    // Collapse an identical event repeated on the very next frame. Input is
    // edge-driven, so a second identical press one frame later is noise that
    // would otherwise inflate the exported macro.
    if (!m_events.empty()) {
        MacroInput& last = m_events.back();
        if (last.down == input.down && last.player2 == input.player2 &&
            last.tick + 1u == input.tick) {
            last.tick = input.tick; // stretch the previous edge forward
            return;
        }
    }

    m_events.push_back(input);
}

// Not noexcept: this allocates (reserve/push_back/string). Declaring noexcept
// would turn an out-of-memory condition into std::terminate instead of a
// recoverable error at the export call site.
MacroData Recorder::snapshot(MacroFormat format) const {
    MacroData data;
    data.format = format;
    data.declaredFps = static_cast<double>(kBaseTps);
    data.frameBased = true;
    data.sourceName = "recorder";

    data.inputs.reserve(m_events.size());
    for (const MacroInput& input : m_events) {
        data.inputs.push_back(input);
        if (input.tick > data.maxTick) data.maxTick = input.tick;
    }
    return data;
}

bool Recorder::exportAs(MacroFormat format, std::vector<std::uint8_t>& out) const {
    return writeMacro(snapshot(format), format, out);
}

bool Recorder::writeTo(const std::filesystem::path& path, MacroFormat format,
                       std::string* errorMessage) const {
    if (format == MacroFormat::Unknown) format = formatForExtension(path);

    if (format == MacroFormat::XBot) {
        if (errorMessage != nullptr) {
            *errorMessage = "xBot has no writer in this build; use XDBot or plain text";
        }
        return false;
    }
    if (format == MacroFormat::Unknown) {
        if (errorMessage != nullptr) *errorMessage = "unrecognised export format";
        return false;
    }

    std::vector<std::uint8_t> bytes;
    if (!writeMacro(snapshot(format), format, bytes)) {
        if (errorMessage != nullptr) *errorMessage = "no writer for the selected format";
        return false;
    }

    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            if (errorMessage != nullptr) {
                *errorMessage = "cannot create '" + path.parent_path().string() + "'";
            }
            return false;
        }
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        if (errorMessage != nullptr) *errorMessage = "cannot open '" + path.string() + "' for writing";
        return false;
    }

    if (!bytes.empty()) {
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    file.flush();
    if (!file) {
        if (errorMessage != nullptr) *errorMessage = "write to '" + path.string() + "' failed";
        return false;
    }
    return true;
}

std::filesystem::path Recorder::defaultExportPath() {
    // Mod::get() is only null if the export is somehow reached before the mod is
    // loaded. "." is used rather than temp_directory_path() because the latter can
    // throw, and a convenience path helper should never do that.
    geode::Mod* mod = geode::Mod::get();
    const std::filesystem::path base =
        mod != nullptr ? mod->getSaveDir() : std::filesystem::path{"."};
    return base / "captures" / "capture.macro";
}

} // namespace afpc