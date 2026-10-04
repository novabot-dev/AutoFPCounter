#pragma once

#include <cstdint>
#include <string>

namespace afpc {

// ---------------------------------------------------------------------------
// Discrete window buckets.
//
// The raw window is an unbounded number of ticks. Display and statistics use
// this fixed set so the on-screen layout never changes shape mid-attempt.
// Windows wider than 12 ticks still increment the last bucket; they are simply
// reported at the same ceiling of "11-12".
// ---------------------------------------------------------------------------
enum class WindowBucket : std::uint8_t {
    One = 0,
    Two,
    Three,
    Four,
    FiveSix,
    SevenEight,
    NineTen,
    ElevenTwelve,
};

inline constexpr int kBucketCount = 8;

[[nodiscard]] const char* bucketLabel(WindowBucket bucket) noexcept;

// ---------------------------------------------------------------------------
// Playback speed mode.
//
// Normal      - one macro tick per engine tick. Input only; physics untouched.
// Accelerated - drives extra engine ticks inside one rendered frame. Opt-in,
//               hard-bounded, and refuses to continue rather than dropping a
//               scheduled input (see PlaybackEngine).
// ---------------------------------------------------------------------------
enum class PlaybackSpeed : std::uint8_t {
    Off = 0,
    Normal = 1,
    Accelerated = 2,
};

[[nodiscard]] const char* speedName(PlaybackSpeed speed) noexcept;

// ---------------------------------------------------------------------------
// State handed to the exception engine alongside the raw window.
// ---------------------------------------------------------------------------
struct WindowContext {
    // Physics cadence modifiers. Any non-1 time warp means the run is not on the
    // vanilla clock, so the 60 FPS override refuses to fire.
    double timeWarp = 1.0;

    // Engine tick the click landed on. On a 240 TPS baseline, ticks that are
    // exact multiples of kTicksPerSixtyHz are the instants a 60 Hz system would
    // sample, which is what makes a 60 Hz frame-perfect window meaningful here.
    std::uint64_t tick = 0;

    // Whether the nearest interaction boundary was a ground/slope transition.
    // Orb and input-state transitions are excluded from the override: they are
    // resolved by the player's own physics, not by a 60 Hz cadence.
    bool boundaryIsGround = false;

    // Live state snapshot at the click.
    bool grounded = false;
    bool onSlope = false;
    bool playerAlive = false;
    bool playing = false;
    PlaybackSpeed speed = PlaybackSpeed::Off;

    // User policy toggle (mod.json -> "60 FPS Window Override").
    bool allowSixtyOverride = true;
};

struct WindowVerdict {
    // Raw tick distance to the nearest interaction boundary, unclamped for the
    // label maths but clamped to [1, kMaxWindowFrames] for bucketing.
    int rawFrames = 1;
    WindowBucket bucket = WindowBucket::One;

    // 240 / frames, computed with a guarded divide.
    double ceilingFps = 240.0;

    // Ready-to-draw string, e.g. "240 FPS", "60 FPS", "34.3 FPS".
    std::string label;

    // True when the edge-case exception engine replaced the raw label.
    bool overridden = false;
};

// ---------------------------------------------------------------------------
// WindowAnalyzer - pure, stateless maths.
//
// Every function is a static so the engine can be unit tested and reasoned
// about without constructing a game session.
// ---------------------------------------------------------------------------
class WindowAnalyzer {
public:
    // 1,2,3,4 -> their own bucket; 5-6, 7-8, 9-10, >=11 -> the paired buckets.
    // Anything below 1 (including "no boundary known") clamps to 1, because a
    // click that lands on a boundary is the tightest possible window.
    [[nodiscard]] static WindowBucket bucketFor(int frames) noexcept;

    // 240 / frames. Returns 0 for a non-positive or non-finite input rather than
    // infinity, so the caller can never divide by it later.
    [[nodiscard]] static double ceilingFpsForFrames(int frames) noexcept;

    // True when the click landed exactly on the 240 TPS tick grid position that a
    // 60 Hz cadence samples, i.e. tick % kTicksPerSixtyHz == 0.
    [[nodiscard]] static bool onSixtyHzGrid(std::uint64_t tick) noexcept;

    // Raw windows of 5..8 ticks map to 48 / 40 / 34.3 / 30 FPS by the 240/N
    // rule. The override replaces that with "60 FPS" when the state data says
    // the press executed on the vanilla 60 Hz cadence.
    //
    // Conditions, all required:
    //   1. the policy toggle is on;
    //   2. the raw window is in [5, 8];
    //   3. time warp is exactly 1.0, i.e. the run is on the vanilla clock;
    //   4. playback speed is Normal - the accelerated driver perturbs the
    //      cadence, so its alignment cannot be trusted;
    //   5. the click landed on the 60 Hz tick grid (tick % 4 == 0);
    //   6. the nearest interaction boundary was a ground/slope transition;
    //   7. the player is alive, playing, and grounded or on a slope.
    //
    // This is a DISPLAY heuristic keyed off the state signals above. It is not
    // derivable from the window size: on a 240 TPS baseline a true 60 Hz
    // frame-perfect window is 4 ticks, not 5-8. See docs/FORMATS.md.
    [[nodiscard]] static bool shouldForceSixtyOverride(int rawFrames,
                                                      const WindowContext& ctx) noexcept;

    [[nodiscard]] static WindowVerdict evaluate(int rawFrames, const WindowContext& ctx);
};

} // namespace afpc