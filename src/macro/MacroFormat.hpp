#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace afpc {

// ---------------------------------------------------------------------------
// Wire formats understood by the importer.
//
// Schema notes below were taken from peonii/nat-converter (the reference
// multi-format converter, maintained by the Silicate author) and cross-checked
// against matcool/gd-macro-converter. See docs/FORMATS.md.
// ---------------------------------------------------------------------------
enum class MacroFormat : std::uint8_t {
    Unknown,
    Silicate,      // binary: f64 fps, u32 count, packed u32 inputs
    XDBot,         // text:   "<fps>" then "frame|hold|button|p2"
    XBot,          // text:   "fps: <n>" / "frames" then "<state> <frame>"  (2.1 legacy)
    MegaHackJson,  // json:   { meta:{fps}, events:[{frame,down,p2,...}] }
    Eclipse,       // text:   "<fps>" / "<count>" then "x, y, delay, down"
    PlainText,     // text:   "<fps>" then "frame hold p2"
};

[[nodiscard]] const char* formatName(MacroFormat fmt) noexcept;
[[nodiscard]] bool formatIsFrameBased(MacroFormat fmt) noexcept;

// One decoded raw input. `tick` is an absolute frame index from the start of
// the macro. `player2` selects which player the press applies to.
struct MacroInput {
    std::uint32_t tick = 0;
    bool down = false;
    bool player2 = false;
};

// A fully decoded macro. `declaredFps` is kept separately from the inputs so
// the 240 FPS gate can inspect the *file's* claim rather than a derived value.
struct MacroData {
    MacroFormat format = MacroFormat::Unknown;
    double declaredFps = 0.0;
    bool frameBased = true; // false => X-position indexed, not frame indexed
    std::vector<MacroInput> inputs;
    std::uint64_t maxTick = 0;
    std::string sourceName;
    std::string diagnostics; // non-fatal notes gathered while parsing

    void clear() noexcept {
        format = MacroFormat::Unknown;
        declaredFps = 0.0;
        frameBased = true;
        maxTick = 0;
        sourceName.clear();
        diagnostics.clear();
        // Contents are dropped; capacity is intentionally retained, so re-using
        // one MacroData for a second import performs zero allocations.
        inputs.clear();
    }
};

enum class MacroError : std::uint8_t {
    None,
    Empty,
    TooLarge,
    UnknownFormat,
    Malformed,
    NonFiniteHeader,
};

struct MacroParseResult {
    bool ok = false;
    MacroError error = MacroError::None;
    std::string message;
    MacroData data;

    static MacroParseResult failure(MacroError err, std::string msg) {
        MacroParseResult r;
        r.ok = false;
        r.error = err;
        r.message = std::move(msg);
        return r;
    }
};

// ---- parsing -------------------------------------------------------------

[[nodiscard]] MacroParseResult parseMacroBuffer(std::vector<std::uint8_t> bytes,
                                                std::string name);

// Reads at most kMaxMacroBytes + 1 so an oversized file is rejected by size
// without ever being fully loaded.
[[nodiscard]] MacroParseResult parseMacroFile(const std::filesystem::path& path);

// Exposed for the unit-testable core and for round-trip verification.
[[nodiscard]] MacroParseResult parseAsSilicate(const std::vector<std::uint8_t>& bytes);
[[nodiscard]] MacroParseResult parseAsXDBot(const std::string& text);
[[nodiscard]] MacroParseResult parseAsXBot(const std::string& text);
[[nodiscard]] MacroParseResult parseAsMegaHackJson(const std::string& text);
[[nodiscard]] MacroParseResult parseAsEclipse(const std::string& text);
[[nodiscard]] MacroParseResult parseAsPlainText(const std::string& text);

[[nodiscard]] MacroFormat detectFormat(const std::vector<std::uint8_t>& bytes);

// ---- serialisation (recorder export + round-trip tests) ------------------

// Writes `data` in `fmt`. Returns false and leaves `out` untouched on failure.
[[nodiscard]] bool writeMacro(const MacroData& data, MacroFormat fmt,
                              std::vector<std::uint8_t>& out);

// ---- 240 FPS ingestion gate ---------------------------------------------

enum class RejectReason : std::uint8_t {
    None,
    ParseFailed,
    FpsNotFinite,
    FpsMissing,
    FpsMismatch,
    NotFrameBased,
    TooManyInputs,
};

struct ValidationResult {
    bool ok = false;
    RejectReason reason = RejectReason::None;
    std::string message;

    static ValidationResult pass() {
        ValidationResult r;
        r.ok = true;
        r.reason = RejectReason::None;
        return r;
    }

    static ValidationResult reject(RejectReason reason, std::string msg) {
        ValidationResult r;
        r.ok = false;
        r.reason = reason;
        r.message = std::move(msg);
        return r;
    }
};

// Strict gate: the file's declared tick rate must be exactly `requiredFps`
// (within `epsilon`), must be finite, and the macro must actually be frame
// indexed. There is deliberately no "closest match" behaviour - a macro
// recorded at 240 but declared as 239.9994 is rejected and reported.
[[nodiscard]] ValidationResult enforceNativeFps(const MacroData& data,
                                                 double requiredFps = static_cast<double>(240),
                                                 double epsilon = 1e-3);

[[nodiscard]] const char* rejectReasonName(RejectReason r) noexcept;

} // namespace afpc