#pragma once

#include <cstddef>
#include <cstdint>

#include "../core/Numeric.hpp"
#include "../macro/MacroFormat.hpp"
#include "../analysis/WindowAnalyzer.hpp"

// GJBaseGameLayer is a Geode binding and lives in the GLOBAL namespace. See the
// note in InteractionTracker.hpp for why this declaration must sit outside `afpc`:
// an `afpc::GJBaseGameLayer` forward declaration would shadow the real binding and
// turn `layer->handleButton(...)` into an error on an incomplete type.
class GJBaseGameLayer;

namespace afpc {

// ---------------------------------------------------------------------------
// PlaybackEngine
//
// CONCURRENCY
// -----------
// This engine creates no threads, takes no locks, and touches no atomics. Every
// method runs on the game's main thread inside the engine's own update, so
// thread contention and lock ordering are not merely unlikely - they are
// structurally impossible. That is the mechanism behind the "no thread blocks
// under high speed execution" property, not an optimisation pass.
//
// TICK DISCIPLINE
// ---------------
// Playback never invents its own tick counter. It is handed the engine tick
// index once per real physics step and delivers exactly the inputs whose frame
// index has arrived. Because input delivery is a forward-only cursor over a
// pre-sorted vector, cost per tick is O(1) amortised with no allocation.
//
// If any condition would force the engine to skip a scheduled input, playback
// does NOT skip it. It latches a desync flag and stops. A macro either runs
// exactly as recorded or it refuses to run.
// ---------------------------------------------------------------------------
class PlaybackEngine {
public:
    void unload() noexcept;

    // Takes ownership of `data`. Resets cursor, counters and clock.
    void load(MacroData&& data) noexcept;

    [[nodiscard]] bool isLoaded() const noexcept { return m_loaded; }
    [[nodiscard]] bool isRunning() const noexcept { return m_running; }

    void start() noexcept;
    void stop() noexcept;
    void restart() noexcept;

    void setSpeed(PlaybackSpeed speed) noexcept;
    [[nodiscard]] PlaybackSpeed speed() const noexcept { return m_speed; }

    // Arm / disarm input delivery. Disarmed while the attempt is paused, the
    // player is dead, or the level has not started. The tick index keeps
    // advancing while disarmed, so a pause cannot desynchronise the macro.
    void setInjectionArmed(bool armed) noexcept { m_armed = armed; }

    // Called exactly once per engine tick, BEFORE the engine's own update runs,
    // so a queued press is consumed by the tick it was recorded for rather than
    // one tick late.
    //
    // `engineTick` is only used to detect that a tick was simulated without
    // playback observing it; the relative tick index comes from m_elapsedTicks,
    // which is reset with the attempt.
    void preTick(GJBaseGameLayer* layer, std::uint64_t engineTick) noexcept;

    // Accelerated path. Feeds real dt to the fixed clock and reports how many
    // extra engine ticks are due. The result is bounded by kMaxStepsPerFrame and
    // the clock keeps the remainder, so capping delays work rather than
    // dropping it.
    [[nodiscard]] int accumulateExtraTicks(float dt) noexcept;

    // True once playback refused to skip a step. Latched until restart/unload.
    [[nodiscard]] bool desynced() const noexcept { return m_desynced; }
    [[nodiscard]] const char* desyncReason() const noexcept { return m_desyncReason; }

    [[nodiscard]] std::uint64_t deliveredInputs() const noexcept { return m_delivered; }
    [[nodiscard]] std::uint64_t elapsedTicks() const noexcept { return m_elapsedTicks; }
    [[nodiscard]] std::size_t cursor() const noexcept { return m_cursor; }
    [[nodiscard]] std::size_t size() const noexcept { return m_macro.inputs.size(); }
    [[nodiscard]] const MacroData& macro() const noexcept { return m_macro; }
    [[nodiscard]] std::uint64_t maxTick() const noexcept { return m_macro.maxTick; }

    // Maximum extra engine ticks per rendered frame in accelerated mode.
    static constexpr int kMaxStepsPerFrame = kMaxDrivenTicksPerFrame;

private:
    void latchDesync(const char* reason) noexcept;
    bool deliverOne(GJBaseGameLayer* layer, const MacroInput& input) noexcept;

    MacroData m_macro;
    std::size_t m_cursor = 0;
    FixedClock m_clock; // accelerated driver cadence only

    // Ticks observed since the attempt began. This is the relative tick index
    // the macro's frame numbers are expressed against.
    std::uint64_t m_elapsedTicks = 0;
    std::uint64_t m_delivered = 0;

    // First engine tick observed after start(), used to detect a tick that was
    // simulated without playback seeing it.
    std::uint64_t m_anchorEngineTick = 0;
    bool m_hasAnchor = false;

    PlaybackSpeed m_speed = PlaybackSpeed::Off;
    bool m_loaded = false;
    bool m_running = false;
    bool m_armed = false;
    bool m_desynced = false;
    const char* m_desyncReason = "";
};

} // namespace afpc