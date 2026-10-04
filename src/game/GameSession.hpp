#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

#include "../analysis/FrameStats.hpp"
#include "../analysis/ClickLog.hpp"
#include "../analysis/InteractionTracker.hpp"
#include "../analysis/WindowAnalyzer.hpp"
#include "../playback/PlaybackEngine.hpp"
#include "../playback/Recorder.hpp"
#include "../ui/Overlay.hpp"

// Both are Geode bindings and live in the GLOBAL namespace. See the note in
// InteractionTracker.hpp for why these declarations must not move inside `afpc`:
// an `afpc::PlayLayer` forward declaration would shadow the real one and turn
// every member access below into a compile error against an incomplete type.
class PlayLayer;
class PlayerObject;

namespace afpc {

// ---------------------------------------------------------------------------
// GameSession
//
// The single owner of every per-attempt subsystem. Hooks in main.cpp are thin:
// they call preEngineTick / postEngineTick / postFrame / onRawButton and this
// class decides what happens.
//
// Ownership and lifetime
// ----------------------
// A single instance exists for the process lifetime (function-local static).
// It owns the overlay nodes, which are cocos2d children of the current
// PlayLayer. attach() claims a layer, detach() releases every node and resets the
// attempt state, so there is no path where a node outlives its layer.
//
// Attempt lifetime
// ----------------
// An attempt is (re)begun whenever the engine's player object identity changes -
// which covers the first spawn, a death and respawn, and every practice-mode
// retry - or on attach. Tick numbering, the boundary tracker, the statistics and
// the recorder all reset together so nothing leaks between attempts.
// ---------------------------------------------------------------------------
class GameSession {
public:
    static GameSession& get() {
        static GameSession instance;
        return instance;
    }

    // -- layer lifecycle ---------------------------------------------------
    bool attach(PlayLayer* layer);
    void detach();

    // Re-reads every mod.json setting. Cheap; called on attach.
    void refreshSettings();

    // Accelerated driver reentrancy latch.
    //
    // This lives here rather than on the $modify class in main.cpp on purpose: a
    // Geode $modify struct is used for member-offset computation and is cast
    // onto the engine's own allocation, so a plain data member declared in it
    // would read past the end of the real object. Adding storage to a modified
    // class requires Geode's `struct Fields` mechanism; the latch is small enough
    // that owning it here is simpler and has no allocation cost.
    //
    // driverEnter() returns false if a driver loop is already active, which is
    // exactly the reentrancy case.
    bool driverEnter() noexcept {
        if (m_driverActive) return false;
        m_driverActive = true;
        return true;
    }
    void driverExit() noexcept { m_driverActive = false; }

    // -- hook entry points ------------------------------------------------
    // Runs before the engine's own update for the tick about to be simulated.
    void preEngineTick(PlayLayer* layer);
    // Runs after the engine's own update, once per simulated tick.
    void postEngineTick(PlayLayer* layer);
    // Runs after the engine's postUpdate, i.e. immediately before rendering.
    // `dt` is forwarded from postUpdate so nothing has to guess at frame time.
    void postFrame(PlayLayer* layer, float dt);
    // Runs for every raw press/release, from the handleButton hook.
    void onRawButton(PlayLayer* layer, bool down, int button, bool isPlayer1);

    // -- macro ingestion --------------------------------------------------
    // Parse + 240 FPS gate + load into the playback engine.
    bool importMacroFile(const std::filesystem::path& path);
    bool importMacroData(MacroData&& data);

    // Scans <saveDir>/macros/ and ingests the first file that passes the gate.
    // Returns the path that was loaded, or an empty path when nothing loaded.
    std::filesystem::path autoLoadFromDisk();

    // -- playback control -------------------------------------------------
    void startPlayback();
    void stopPlayback();
    void restartPlayback();
    void setPlaybackSpeed(PlaybackSpeed speed);

    // Applies one of the kSpeedPresets entries. Index 0 (1.0x) maps to
    // PlaybackSpeed::Normal; every other entry maps to Accelerated with the
    // preset as its rate. Out-of-range indices are ignored.
    void setSpeedPreset(int index);
    [[nodiscard]] int activeSpeedPreset() const noexcept { return m_speedPreset; }

    // Requested and realised playback rate. requestedPlaybackRate() is what the
    // user picked; realisedPlaybackRate() is measured from ticks actually driven
    // and is what the UI shows, because the per-frame ceiling means a large
    // requested rate is not reachable.
    [[nodiscard]] double requestedPlaybackRate() const noexcept { return m_playback.rate(); }
    [[nodiscard]] double realisedPlaybackRate() const noexcept { return m_playback.effectiveRate(); }

    // Whole-tick shift applied to injected clicks. See PlaybackEngine::setClickOffset.
    void setClickOffset(int ticks);
    [[nodiscard]] int clickOffset() const noexcept { return m_playback.clickOffset(); }

    // -- click log + window overrides -------------------------------------
    //
    // The log is per attempt: beginAttempt() clears it. Overrides are applied to
    // the log's copy of each click and the aggregate tallies are then rebuilt
    // from it, so the corner counters can never disagree with what the FRAMES
    // list is showing.
    [[nodiscard]] ClickLog& clickLog() noexcept { return m_clickLog; }
    [[nodiscard]] const ClickLog& clickLog() const noexcept { return m_clickLog; }

    void setClickWindowOverride(std::uint64_t sequence, int frames);
    void clearClickWindowOverride(std::uint64_t sequence);
    void rebuildStatsFromClicks();

    // -- recording --------------------------------------------------------
    // `format` may be MacroFormat::Unknown, in which case the writer is inferred
    // from the file extension.
    bool exportRecording(const std::filesystem::path& path,
                         MacroFormat format = MacroFormat::Unknown,
                         std::string* errorMessage = nullptr);

    // -- accessors --------------------------------------------------------
    [[nodiscard]] PlaybackEngine& playback() noexcept { return m_playback; }
    [[nodiscard]] const PlaybackEngine& playback() const noexcept { return m_playback; }
    [[nodiscard]] Recorder& recorder() noexcept { return m_recorder; }
    [[nodiscard]] const Recorder& recorder() const noexcept { return m_recorder; }
    [[nodiscard]] FrameStats& stats() noexcept { return m_stats; }
    [[nodiscard]] Overlay& overlay() noexcept { return m_overlay; }
    [[nodiscard]] const Overlay& overlay() const noexcept { return m_overlay; }
    [[nodiscard]] std::uint64_t tick() const noexcept { return m_tick; }
    [[nodiscard]] bool attemptActive() const noexcept { return m_attemptActive; }
    [[nodiscard]] bool overlayEnabled() const noexcept { return m_settings.overlay; }
    [[nodiscard]] const std::string& macroDiagnostics() const noexcept { return m_diagnostics; }

private:
    struct Settings {
        bool overlay = true;
        bool recorder = true;
        bool rejectNon240 = true;
        bool sixtyOverride = true;
        bool accelerated = false;
        bool boundaryDots = true;
        bool autoStart = true;
    };

    // A click awaiting window analysis. Captured at the input event itself so
    // the position is the true click position, not a later reconstruction.
    struct PendingClick {
        bool valid = false;
        std::uint64_t tick = 0;
        float worldX = 0.f;
        float worldY = 0.f;
        bool boundaryIsGround = false;
        bool hasBoundary = false;
        int rawFrames = 1;
        PlaybackSpeed speedAtInput = PlaybackSpeed::Off;
    };

    static constexpr int kMaxPendingClicks = 8;

    void beginAttempt() noexcept;
    void endAttempt() noexcept;
    void queueClick(PlayLayer* layer, std::uint64_t tick, PlaybackSpeed speedAtInput) noexcept;
    void drainPendingClicks(PlayLayer* layer) noexcept;
    [[nodiscard]] WindowContext buildContext(PlayLayer* layer, const PendingClick& click) const noexcept;
    void queueAlert(std::string text);
    void drainAlert();
    void reportPlaybackDesync();

    PlayLayer* m_layer = nullptr;
    const PlayerObject* m_lastPlayer = nullptr;

    Settings m_settings{};

    PlaybackEngine m_playback;
    Recorder m_recorder;
    InteractionTracker m_tracker;
    FrameStats m_stats;
    ClickLog m_clickLog;
    Overlay m_overlay;

    std::array<PendingClick, kMaxPendingClicks> m_pending{};
    int m_pendingCount = 0;

    std::string m_statsBuffer; // reused; steady state allocates nothing
    std::string m_diagnostics;
    std::string m_alertText;
    bool m_alertShown = false;

    // Latched once a playback desync has been surfaced, so the report is shown
    // exactly once per run instead of every frame after the latch trips.
    bool m_desyncReported = false;

    // Index into kSpeedPresets, or -1 when playback speed was set
    // programmatically rather than chosen from the preset grid. refreshSettings()
    // seeds it from the mod.json toggle on first run.
    int m_speedPreset = -1;

    std::uint64_t m_tick = 0;
    bool m_attemptActive = false;
    bool m_driverActive = false;

    // Cursor for the overlay's boundary dots: the last engine tick whose boundaries
    // have already been handed to the overlay. Reset with the attempt, and starts
    // at kNoBoundaryCursor so the attempt's tick-0 origin boundary is drawn too.
    std::uint64_t m_lastDrawnBoundary = InteractionTracker::kNoBoundaryCursor;
    std::array<InteractionTracker::BoundaryPoint, Overlay::kMaxBoundaryDots> m_boundaryScratch{};
};

} // namespace afpc