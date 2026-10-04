#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>

namespace afpc {

// ---------------------------------------------------------------------------
// Baseline simulation constants.
//
// Everything the maths engine does is expressed relative to a fixed 240 tick
// per second physics baseline. Frame windows are counted in whole ticks and
// only converted to a frame-rate ceiling at the very last step, so no floating
// point value is ever carried across a frame boundary.
// ---------------------------------------------------------------------------

inline constexpr int kBaseTps = 240;
inline constexpr double kBaseTpsD = 240.0;

// One physics tick, in seconds. Never used for accumulation - see FixedClock.
inline constexpr double kTickSeconds = 1.0 / 240.0;

// 2*pi. Provided locally so we never depend on M_PI being defined.
inline constexpr double kTwoPi = 6.283185307179586476925286766559;

// A 60 Hz cadence on a 240 TPS baseline is exactly 4 ticks.
inline constexpr int kTicksPerSixtyHz = kBaseTps / 60;

// Window buckets are capped at 12 ticks for classification / display.
// Larger real windows are still counted, they just land in the last bucket.
inline constexpr int kMaxWindowFrames = 12;

// Bounds used to reject corrupt / hostile headers before any arithmetic.
inline constexpr double kMinPlausibleTps = 1.0;
inline constexpr double kMaxPlausibleTps = 100000.0;

// Hard ceiling on an ingested macro file. 64 MiB is far beyond any real macro
// (a 1 hour 240 FPS macro is ~1.5 MiB) and stops a malformed file from
// causing a huge allocation.
inline constexpr std::size_t kMaxMacroBytes = 64u * 1024u * 1024u;

// Hard ceiling on decoded inputs, independent of file size, so a file that
// declares a huge count but is short cannot drive a huge reserve().
inline constexpr std::size_t kMaxMacroInputs = 8u * 1000u * 1000u;

// Real delta-time clamp. Geometry Dash can hand out enormous dt values when the
// window is dragged or the machine stalls; clamping keeps the fixed clock from
// fast-forwarding through hundreds of ticks in one frame.
inline constexpr double kMaxRealDt = 0.25;

// ---------------------------------------------------------------------------
// Accelerated playback bounds.
//
// Accelerated mode calls PlayLayer::update() extra times inside one rendered
// frame. The loop is bounded so a single frame can never stall, and the macro
// cursor is never allowed to run ahead of the ticks that were actually
// simulated - the engine latches a desync flag instead of silently skipping.
// ---------------------------------------------------------------------------

// Requested timescale multipliers for accelerated playback, offered as presets.
//
// The multiplier is the *intent*; kMaxDrivenTicksPerFrame is the limit, and the
// limit always wins. At 60 FPS rendering the cap allows 16 * 60 = 960 ticks per
// real second, i.e. exactly 4x the 240 TPS baseline. Every preset above 4 is
// therefore unreachable in a single rendered frame no matter what is requested,
// which is why the UI reports the achieved rate next to the requested one rather
// than showing the requested figure alone.
inline constexpr double kSpeedPresets[] = {
    1.0,  2.0,  3.0,  4.0,  6.0,  8.0,  12.0,  16.0,  24.0,
    32.0, 48.0, 64.0, 73.0, 96.0, 128.0, 150.0, 183.0, 200.0,
};

inline constexpr int kSpeedPresetCount = static_cast<int>(std::size(kSpeedPresets));

// A 1.0x preset is the vanilla clock and maps to PlaybackSpeed::Normal; every
// other preset maps to Accelerated with kSpeedPresets[i] as its rate.
inline constexpr double kNormalRate = 1.0;

// Hard cap on extra engine ticks driven inside one rendered frame.
inline constexpr int kMaxDrivenTicksPerFrame = 16;

// Largest whole-tick shift the UI will apply to injected clicks. Bounded by the
// window search radius: shifting further than the tracker searches could not
// produce a different measured window, so it would be indistinguishable from the
// clamp.
inline constexpr int kMaxClickOffsetTicks = 12;

// Smoothing factor for the measured (achieved) playback rate. Low enough that
// the readout is stable frame to frame, high enough that a genuine speed change
// shows up within about a second.
inline constexpr double kRateSmoothingAlpha = 0.1;

// How many ticks the window search will look backwards/forwards from a click.
inline constexpr int kWindowSearchRadius = 12;

// Lifetime of an on-screen click marker, in seconds.
inline constexpr double kMarkerLifetime = 0.35;

// Capacity of the interaction-boundary ring buffer. Power-of-two is not required
// (the wrap maths uses a modulo, not a mask) but it keeps the slot arithmetic
// cheap.
inline constexpr int kBoundaryRingCapacity = 1024;

} // namespace afpc