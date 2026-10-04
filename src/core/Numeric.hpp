#pragma once

#include <cmath>
#include <cstdint>
#include <string>

#include "Constants.hpp"

namespace afpc {

// ---------------------------------------------------------------------------
// Floating point hardening.
//
// The window analysis engine is fed values that come from three sources the
// mod does not control: the engine's dt accumulator, parsed macro headers, and
// live gameplay state. Any of them can be NaN, +/-Inf, zero, or wildly out of
// range. Every conversion in this project goes through the helpers below so a
// single bad value degrades one measurement instead of poisoning the maths
// engine.
// ---------------------------------------------------------------------------

inline constexpr double kInvalidDouble = -1.0;

// Division that cannot produce inf or NaN. A zero (or non finite) denominator
// yields `fallback` instead.
[[nodiscard]] inline double safeDivide(double num, double den,
                                       double fallback = kInvalidDouble) noexcept {
    if (!std::isfinite(num) || !std::isfinite(den) || den == 0.0) return fallback;
    const double r = num / den;
    return std::isfinite(r) ? r : fallback;
}

// Saturating conversion from a double that may be garbage to an int.
[[nodiscard]] inline int saturatingInt(double v, int lo, int hi, int fallback) noexcept {
    if (!std::isfinite(v)) return fallback;
    if (v <= static_cast<double>(lo)) return lo;
    if (v >= static_cast<double>(hi)) return hi;
    return static_cast<int>(v);
}

// ---------------------------------------------------------------------------
// FixedClock
//
// A monotonic tick generator backed by an *integer* nanosecond accumulator.
//
// Rationale: at 240 TPS a tick is 4166666.66... ns, which is not
// representable in binary floating point. Accumulating dt in double drifts
// measurably over a long attempt, and the drift shows up directly as an
// off-by-one in click frames. Accumulating in int64 nanoseconds makes the
// long-run tick count exact and makes the sub-tick phase a pure remainder
// computation with no drift at all.
// ---------------------------------------------------------------------------
class FixedClock {
public:
    void reset() noexcept {
        m_tickNs = 1'000'000'000ll / kBaseTps; // 4166666 ns, exact at 240 TPS
        m_accumNs = 0;
        m_tick = 0;
        m_speed = 1.0;
    }

    // Feed one rendered frame of wall-clock time, scaled by `speed`.
    // Returns the number of whole ticks that are now available to simulate.
    // `maxSteps` bounds the return value; the remainder is *kept*, never
    // dropped, so capping can only delay work - it can never desynchronise.
    int accumulate(double realDtSeconds, double speed, int maxSteps) noexcept {
        m_speed = speed;

        double dt = realDtSeconds;
        if (!std::isfinite(dt) || dt < 0.0) dt = 0.0;
        if (dt > kMaxRealDt) dt = kMaxRealDt;

        if (!(speed > 0.0) || !std::isfinite(speed)) return 0; // hold position exactly

        dt *= speed;

        // dt is now bounded by kMaxRealDt * speed. Guard the multiply anyway.
        double ns = dt * 1e9;
        if (!std::isfinite(ns)) return 0;
        if (ns < 0.0) ns = 0.0;
        if (ns > static_cast<double>(kAccumulatorCeilingNs)) {
            ns = static_cast<double>(kAccumulatorCeilingNs);
        }

        m_accumNs += static_cast<std::int64_t>(ns);
        if (m_accumNs > kAccumulatorCeilingNs) m_accumNs = kAccumulatorCeilingNs;

        int steps = 0;
        const int limit = maxSteps < 0 ? 0 : maxSteps;
        while (steps < limit && m_accumNs >= m_tickNs) {
            m_accumNs -= m_tickNs;
            ++m_tick;
            ++steps;
        }
        return steps;
    }

    [[nodiscard]] std::uint64_t tick() const noexcept { return m_tick; }
    [[nodiscard]] std::int64_t remainderNs() const noexcept { return m_accumNs; }
    [[nodiscard]] std::int64_t tickNs() const noexcept { return m_tickNs; }

    // Last timescale multiplier this clock was fed. Diagnostic only.
    [[nodiscard]] double speed() const noexcept { return m_speed; }

private:
    // Absolute ceiling on the backlog, so a pathological speed multiplier can
    // never overflow the int64 accumulator. Four seconds is far more catch-up
    // than any real frame needs.
    static constexpr std::int64_t kAccumulatorCeilingNs = 4'000'000'000ll;

    std::int64_t m_tickNs = 4'166'666;
    std::int64_t m_accumNs = 0;
    std::uint64_t m_tick = 0;
    double m_speed = 1.0;
};

// ---------------------------------------------------------------------------
// Formatting helpers. Only called on discrete events (a click), never per tick,
// so the small allocations here are irrelevant to frame time.
// ---------------------------------------------------------------------------

// "240 FPS", "60 FPS", "34.3 FPS". Exact integers stay exact; everything else is
// truncated to one decimal, because a ceiling must never be rounded *up* past
// the next bucket. Not noexcept: std::to_string allocates, and declaring
// noexcept would turn a bad_alloc into std::terminate.
[[nodiscard]] inline std::string formatFpsLabel(double fps) {
    if (!std::isfinite(fps) || fps <= 0.0) return "N/A FPS";

    const double rounded = std::floor(fps + 0.5);
    if (std::fabs(fps - rounded) < 1e-9) {
        return std::to_string(static_cast<long long>(rounded)) + " FPS";
    }

    // One decimal, truncated (never rounded up into the next bucket).
    long long tenths = static_cast<long long>(fps * 10.0);
    if (tenths < 0) tenths = 0;
    const long long whole = tenths / 10;
    const long long frac = tenths % 10;

    std::string out = std::to_string(whole);
    out += '.';
    out += static<char>('0' + frac);
    out += " FPS";
    return out;
}

} // namespace afpc