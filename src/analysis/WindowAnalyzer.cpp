#include "WindowAnalyzer.hpp"

#include <cmath>

#include "../core/Constants.hpp"
#include "../core/Numeric.hpp"

namespace afpc {

const char* bucketLabel(WindowBucket bucket) noexcept {
    switch (bucket) {
        case WindowBucket::One: return "1";
        case WindowBucket::Two: return "2";
        case WindowBucket::Three: return "3";
        case WindowBucket::Four: return "4";
        case WindowBucket::FiveSix: return "5-6";
        case WindowBucket::SevenEight: return "7-8";
        case WindowBucket::NineTen: return "9-10";
        case WindowBucket::ElevenTwelve: return "11-12";
    }
    return "1";
}

const char* speedName(PlaybackSpeed speed) noexcept {
    switch (speed) {
        case PlaybackSpeed::Off: return "off";
        case PlaybackSpeed::Normal: return "normal";
        case PlaybackSpeed::Accelerated: return "accelerated";
    }
    return "off";
}

WindowBucket WindowAnalyzer::bucketFor(int frames) noexcept {
    // "No boundary known" and every sub-tick anomaly collapse to the tightest
    // bucket rather than being discarded: a click with no measurable slack is
    // treated as a 1 frame window.
    if (frames <= 1) return WindowBucket::One;
    if (frames == 2) return WindowBucket::Two;
    if (frames == 3) return WindowBucket::Three;
    if (frames == 4) return WindowBucket::Four;
    if (frames <= 6) return WindowBucket::FiveSix;
    if (frames <= 8) return WindowBucket::SevenEight;
    if (frames <= 10) return WindowBucket::NineTen;
    return WindowBucket::ElevenTwelve;
}

double WindowAnalyzer::ceilingFpsForFrames(int frames) noexcept {
    // 240 / N. frames is an integer that has already been clamped to >= 1 by
    // bucketFor's contract, but the guard here is deliberate: this is the one
    // place in the mod where a division happens, and it must be impossible for
    // it to produce inf or NaN regardless of what the caller passed.
    const int safeFrames = saturatingInt(static_cast<double>(frames), 1, 1 << 20, 1);
    const double result = safeDivide(kBaseTpsD, static_cast<double>(safeFrames), 0.0);
    if (!std::isfinite(result) || result <= 0.0) return 0.0;
    return result;
}

bool WindowAnalyzer::onSixtyHzGrid(std::uint64_t tick) noexcept {
    // 240 / 60 == 4. Every fourth tick is the instant a 60 Hz system samples, so
    // a press on one of those ticks is genuinely resolved by the 60 Hz cadence
    // rather than by the 240 TPS one.
    return (tick % static_cast<std::uint64_t>(kTicksPerSixtyHz)) == 0u;
}

bool WindowAnalyzer::shouldForceSixtyOverride(int rawFrames, const WindowContext& ctx) noexcept {
    if (!ctx.allowSixtyOverride) return false;

    // The rule only speaks about 5..8 frame windows.
    if (rawFrames < 5 || rawFrames > 8) return false;

    // (3) Vanilla clock.
    if (!std::isfinite(ctx.timeWarp) || std::fabs(ctx.timeWarp - 1.0) > 1e-9) return false;

    // (4) The accelerated driver perturbs the cadence, so a press measured under
    //     it carries no information about the 60 Hz grid.
    if (ctx.speed != PlaybackSpeed::Normal) return false;

    // (5) Cadence alignment.
    if (!onSixtyHzGrid(ctx.tick)) return false;

    // (6) Surface-anchored window only.
    if (!ctx.boundaryIsGround) return false;

    // (7) Live sanity.
    if (!ctx.playerAlive || !ctx.playing) return false;
    if (!ctx.grounded && !ctx.onSlope) return false;

    return true;
}

WindowVerdict WindowAnalyzer::evaluate(int rawFrames, const WindowContext& ctx) {
    WindowVerdict verdict;

    // Clamp for bucketing. Windows beyond 12 keep counting, they just share the
    // widest bucket, so the display string never implies more precision than the
    // classification supports.
    const int clampedFrames = saturatingInt(static_cast<double>(rawFrames), 1, kMaxWindowFrames, 1);

    verdict.rawFrames = clampedFrames;
    verdict.bucket = bucketFor(clampedFrames);
    verdict.ceilingFps = ceilingFpsForFrames(clampedFrames);

    // Note the override never touches verdict.bucket. The statistics list reports
    // the window that was measured; only the drawn string is replaced, so the
    // "Frame Window: Count" tally keeps telling the truth about what happened.
    verdict.overridden = shouldForceSixtyOverride(clampedFrames, ctx);
    if (verdict.overridden) {
        // Forced value, not a computed one. 60 Hz on a 240 TPS baseline.
        verdict.ceilingFps = 60.0;
        verdict.label = "60 FPS";
    } else {
        verdict.label = formatFpsLabel(verdict.ceilingFps);
    }

    return verdict;
}

} // namespace afpc