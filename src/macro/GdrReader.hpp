#pragma once

// ---------------------------------------------------------------------------
// GDR - the GDevelop / maxnut replay format, in both of its dialects.
//
// The format is versioned by a field, *not* by the file extension, and the two
// versions are not related by a field swap:
//
//   v1  (.gdr)   a serialised JSON document. The canonical on-disk form is
//                MessagePack (`nlohmann::json::to_msgpack`), with plain JSON
//                accepted as a fallback. Keys: gameVersion, description,
//                version, duration, bot{name,version}, level{id,name}, author,
//                seed, coins, ldm, framerate (optional), inputs[]. Each input is
//                {frame, btn, "2p", down}.
//
//   v2  (.gdr2)  a hand-packed binary stream beginning with the three ASCII
//                bytes "GDR". Fixed-width fields are BIG endian; every integral
//                field (including bool and the input chunk) is LEB128 varint.
//                Strings are length-prefixed, *not* NUL terminated.
//
// Both dialects declare their tick rate explicitly, which is what the 240 FPS
// ingestion gate needs: `declaredFps` is taken verbatim from the file rather
// than assumed.
//
// This reader is intentionally standalone and dependency-free. The upstream
// reference implementation is header-only and requires nlohmann/json plus a
// msgpack backend for v1, which AutoFPCount deliberately does not take on.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace afpc::gdr {

// Why a GDR stream could not be decoded. Reported to the user verbatim, so
// each name is phrased as something worth reading.
enum class Error : std::uint8_t {
    None,
    Truncated,        // ran off the end of the buffer mid-field
    BadMagic,         // not a GDR v2 stream
    UnsupportedVersion,
    LengthOverflow,   // a varint or length prefix exceeds the buffer
    MalformedInputs,  // input array present but not the expected shape
    TooManyInputs,    // input count exceeds the importer's safety ceiling
};

// A single decoded input. `button` is kept because v2 distinguishes jump (1),
// left (2) and right (3), and the 2-player split in v2 is positional rather
// than per-record.
struct Input {
    std::uint64_t frame = 0;
    std::uint8_t button = 1;
    bool player2 = false;
    bool down = false;
};

struct Replay {
    int version = 0;
    std::string author;
    std::string description;
    std::string botName;
    std::string botVersion;
    std::uint32_t levelId = 0;
    std::string levelName;

    double duration = 0.0;   // seconds
    double framerate = 0.0;  // ticks per second, as declared by the file
    int gameVersion = 0;
    int seed = 0;
    int coins = 0;
    bool ldm = false;
    bool platformer = false;

    // Frames at which the player died. Delta encoded on disk.
    std::vector<std::uint64_t> deaths;

    std::vector<Input> inputs;
};

// True when `bytes` starts with the v2 "GDR" magic. Used by the format sniffer;
// v1 has no magic and is identified structurally instead.
[[nodiscard]] bool looksLikeBinary(const std::vector<std::uint8_t>& bytes) noexcept;

// True when `text` looks like a v1 JSON document rather than some other JSON
// dialect: it must be an object carrying the GDR-specific `inputs` array.
[[nodiscard]] bool looksLikeJsonDocument(std::string_view text) noexcept;

// Decodes either dialect, choosing by content rather than by extension.
// `maxInputs` bounds the decoded input count so a corrupt length prefix cannot
// drive an unbounded allocation.
[[nodiscard]] bool parse(const std::vector<std::uint8_t>& bytes, Replay& out,
                         Error& err, std::string& message,
                         std::size_t maxInputs);

} // namespace afpc::gdr
