#include "PlaybackEngine.hpp"

#include <algorithm>

#include <Geode/binding/GJBaseGameLayer.hpp>

namespace afpc {

namespace {

// Geometry Dash's jump button id. Only this button is ever injected, which is
// what every supported macro format records.
constexpr int kJumpButton = 1;

} // namespace

void PlaybackEngine::unload() noexcept {
    m_macro.clear();
    m_cursor = 0;
    m_clock.reset();
    m_elapsedTicks = 0;
    m_delivered = 0;
    m_anchorEngineTick = 0;
    m_hasAnchor = false;
    m_speed = PlaybackSpeed::Off;
    m_loaded = false;
    m_running = false;
    m_armed = false;
    m_desynced = false;
    m_desyncReason = "";
}

void PlaybackEngine::load(MacroData&& data) noexcept {
    unload();
    m_macro = std::move(data);
    m_loaded = true;
    if (m_macro.format == MacroFormat::Unknown) {
        latchDesync("macro format could not be determined");
    }
}

void PlaybackEngine::start() noexcept {
    if (!m_loaded) return;
    m_elapsedTicks = 0;
    m_hasAnchor = false;
    m_running = true;
}

void PlaybackEngine::stop() noexcept {
    m_running = false;
}

void PlaybackEngine::restart() noexcept {
    if (!m_loaded) return;
    m_cursor = 0;
    m_delivered = 0;
    m_elapsedTicks = 0;
    m_anchorEngineTick = 0;
    m_hasAnchor = false;
    m_desynced = false;
    m_desyncReason = "";
    m_clock.reset();
    m_effectiveRate = 0.0;
    m_running = true;
}

void PlaybackEngine::setRate(double rate) noexcept {
    // Guarded rather than clamped to a range: anything non-finite would poison
    // the fixed clock, so fall back to the vanilla cadence instead.
    if (!(rate >= 1.0) || rate > 1e6) return;
    m_rate = rate;
}

void PlaybackEngine::setClickOffset(int ticks) noexcept {
    if (ticks > kMaxClickOffsetTicks) ticks = kMaxClickOffsetTicks;
    if (ticks < -kMaxClickOffsetTicks) ticks = -kMaxClickOffsetTicks;
    m_clickOffset = ticks;
}

void PlaybackEngine::setSpeed(PlaybackSpeed speed) noexcept {
    m_speed = speed;
    if (speed == PlaybackSpeed::Off) {
        m_running = false;
        m_clock.reset();
    }
}

int PlaybackEngine::accumulateExtraTicks(float dt) noexcept {
    if (m_speed != PlaybackSpeed::Accelerated) return 0;

    // m_rate is the requested timescale; kMaxStepsPerFrame is the per-frame
    // ceiling. In practice the ceiling binds first (16 ticks * 60 fps
    // = 960 ticks/s = 4x), and FixedClock keeps whatever it could not drain so
    // the cap delays work instead of discarding it.
    const int due = m_clock.accumulate(static_cast<double>(dt), m_rate, kMaxStepsPerFrame);

    // Measured from what actually came out, not from what was requested. The
    // frame's own tick is not counted: this is *extra* ticks on top of the
    // baseline the engine already ran, so the extra ticks are divided by the
    // baseline ticks the same real interval would have produced on its own.
    // At 60 FPS that baseline is kBaseTps/60 = 4 ticks, so 16 extra ticks read
    // back as 16/4 = 4x.
    const double seconds = static_cast<double>(dt);
    if (seconds > 0.0 && due > 0) {
        const double baseline = kBaseTpsD * seconds;
        const double instantaneous = static_cast<double>(due) / baseline;
        // Smoothed so a single frame in which the ceiling bound does not make the
        // readout flicker between neighbouring values.
        m_effectiveRate = m_effectiveRate > 0.0
                              ? m_effectiveRate + (instantaneous - m_effectiveRate) * kRateSmoothingAlpha
                              : instantaneous;
    }

    return due;
}

void PlaybackEngine::latchDesync(const char* reason) noexcept {
    if (m_desynced) return;
    m_desynced = true;
    m_desyncReason = reason != nullptr ? reason : "unspecified";
    m_running = false;
}

bool PlaybackEngine::deliverOne(GJBaseGameLayer* layer, const MacroInput& input) noexcept {
    if (layer == nullptr) {
        latchDesync("playback target layer was destroyed mid-run");
        return false;
    }

    // handleButton is the engine's own input entry point: it is what the
    // keyboard, touch, and macro systems all funnel through, and it queues the
    // command for consumption by the tick that is about to run. Going through it
    // rather than poking m_queuedButtons directly means playback obeys every
    // piece of engine-side input bookkeeping.
    layer->handleButton(input.down, kJumpButton, !input.player2);
    ++m_delivered;
    return true;
}

void PlaybackEngine::preTick(GJBaseGameLayer* layer, std::uint64_t engineTick) noexcept {
    if (!m_running || !m_loaded || m_desynced) return;

    // Desync guard. Every simulated tick must reach preTick exactly once, so the
    // engine tick we are told about must be precisely the anchor tick plus the
    // number of preTicks so far.
    //
    // A larger value means the engine advanced past us: a macro input scheduled
    // for a tick we never saw would silently never fire. A smaller value means
    // preTick ran twice for one tick, which would make the relative index run
    // ahead of the engine and deliver inputs one tick early. Either way the run
    // is no longer trustworthy, so stop and say so instead of guessing.
    const std::uint64_t expected = m_anchorEngineTick + m_elapsedTicks;
    if (!m_hasAnchor) {
        m_anchorEngineTick = engineTick;
        m_elapsedTicks = 0;
        m_hasAnchor = true;
    } else if (engineTick != expected) {
        latchDesync(engineTick > expected
                        ? "the engine simulated ticks that playback never saw"
                        : "playback observed the same engine tick twice");
        return;
    }

    // Relative tick index. m_elapsedTicks is incremented exactly once per
    // preTick and is reset with the attempt, so it stays in lockstep with the
    // engine tick the macro was recorded against.
    const std::uint64_t relative = m_elapsedTicks;
    ++m_elapsedTicks;

    if (m_speed == PlaybackSpeed::Off) return;
    if (!m_armed) return;

    if (m_cursor >= m_macro.inputs.size()) {
        m_running = false;
        return;
    }

    // Several inputs may share one frame (a press and a release recorded inside
    // the same tick). They are all delivered in order before physics consumes
    // the queue, so ordering is preserved exactly.
    //
    // The click offset is applied here, at the moment of delivery, rather than by
    // rewriting the macro on load: the recorded vector stays pristine, and the
    // shift is visible to the caller through clickOffset(). `due` is computed in
    // signed arithmetic and floored at zero, so a click pushed back before the
    // start of the attempt fires on tick 0 instead of silently never firing.
    std::int64_t due = static_cast<std::int64_t>(relative) - m_clickOffset;
    if (due < 0) due = 0;

    while (m_cursor < m_macro.inputs.size()) {
        const MacroInput& input = m_macro.inputs[m_cursor];
        if (static_cast<std::int64_t>(input.tick) > due) break;
        ++m_cursor;
        if (!deliverOne(layer, input)) return;
    }

    if (m_cursor >= m_macro.inputs.size()) {
        // Macro fully delivered. Do not clear the desync latch: whether the run
        // was clean is a fact the caller reports, not something playback hides.
        m_running = false;
    }
}

} // namespace afpc