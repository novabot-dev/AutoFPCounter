#include "GameSession.hpp"

#include <algorithm>
#include <cctype>
#include <vector>

#include <Geode/binding/FLAlertLayer.hpp>
#include <Geode/binding/GJGameState.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/loader/Log.hpp>
#include <Geode/loader/Mod.hpp>

#include "../core/Constants.hpp"
#include "../core/Numeric.hpp"
#include "../macro/MacroFormat.hpp"

using namespace geode::prelude;

namespace afpc {

namespace {

// Geometry Dash's jump button id. Every supported macro format records only this
// button, so anything else is noise from the input layer.
constexpr int kJumpButton = 1;

constexpr const char* kMacroFolder = "macros";
constexpr const char* kActiveMacroName = "active.macro";

bool settingEnabled(const char* key, bool fallback) {
    // getSettingEnabled() is only meaningful once the mod is loaded. Outside of
    // that window the caller's default is the correct answer.
    Mod* mod = Mod::get();
    if (mod == nullptr) return fallback;
    // getSettingValue<T> yields a value-initialised T() for an unknown key, so a
    // missing key would silently read as `false`. Check first and keep the
    // caller's default in that case.
    if (!mod->hasSetting(key)) return fallback;
    return mod->getSettingValue<bool>(key);
}

} // namespace

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

void GameSession::refreshSettings() {
    m_settings.overlay = settingEnabled("enable-overlay", true);
    m_settings.recorder = settingEnabled("enable-recorder", true);
    m_settings.rejectNon240 = settingEnabled("reject-non-240", true);
    m_settings.sixtyOverride = settingEnabled("force-60-override", true);
    m_settings.accelerated = settingEnabled("enable-accelerated", false);
    m_settings.boundaryDots = settingEnabled("show-boundary-dots", true);
    m_settings.autoStart = settingEnabled("auto-start-playback", true);

    m_overlay.setVisible(m_settings.overlay);

    // Seeds the rate from the mod.json toggle, which is the historical default
    // (accelerated meant a fixed 4x). From here on the preset grid owns it, so
    // this only runs on settings refresh rather than on every attempt start.
    if (m_speedPreset < 0) {
        m_speedPreset = m_settings.accelerated ? 3 : 0;
        m_playback.setRate(kSpeedPresets[m_speedPreset]);
    }
}

// ---------------------------------------------------------------------------
// Layer lifecycle
// ---------------------------------------------------------------------------

bool GameSession::attach(PlayLayer* layer) {
    if (layer == nullptr) return false;

    if (m_layer != nullptr && m_layer != layer) detach();

    m_layer = layer;
    refreshSettings();

    m_attemptActive = false;
    m_lastPlayer = nullptr;
    beginAttempt();

    const bool overlayOk = m_settings.overlay ? m_overlay.attach(layer) : true;
    if (!overlayOk) {
        log::warn("overlay nodes could not be created; continuing without the UI layer");
        m_settings.overlay = false;
        m_overlay.setVisible(false);
    }

    autoLoadFromDisk();
    return true;
}

void GameSession::detach() {
    m_overlay.detach();

    m_pendingCount = 0;
    for (PendingClick& click : m_pending) click = PendingClick{};

    m_recorder.end();
    m_playback.setSpeed(PlaybackSpeed::Off);
    m_playback.setInjectionArmed(false);

    m_tracker.reset();
    m_stats.reset();
    m_statsBuffer.clear();

    m_tick = 0;
    m_attemptActive = false;
    m_driverActive = false;
    m_lastDrawnBoundary = InteractionTracker::kNoBoundaryCursor;
    m_lastPlayer = nullptr;
    m_layer = nullptr;
}

// ---------------------------------------------------------------------------
// Attempt lifecycle
// ---------------------------------------------------------------------------

void GameSession::beginAttempt() noexcept {
    m_attemptActive = true;
    m_tick = 0;
    m_lastDrawnBoundary = InteractionTracker::kNoBoundaryCursor;
    m_pendingCount = 0;
    for (PendingClick& click : m_pending) click = PendingClick{};

    m_tracker.reset();
    m_stats.reset();
    m_clickLog.reset();

    if (m_settings.recorder) m_recorder.begin();
    else m_recorder.end();

    m_playback.restart();
    m_desyncReported = false;
    if (m_playback.isLoaded() && m_settings.autoStart) {
        // Re-applied from the live rate rather than from the mod.json toggle: the
        // preset and the click offset are controls the user drives mid-session,
        // and a practice-mode retry must not silently discard them.
        m_playback.setSpeed(m_playback.rate() > 1.0 ? PlaybackSpeed::Accelerated
                                                    : PlaybackSpeed::Normal);
        m_playback.start();
    } else {
        m_playback.setSpeed(PlaybackSpeed::Off);
    }
}

void GameSession::endAttempt() noexcept {
    m_attemptActive = false;
    m_recorder.end();
    m_playback.setInjectionArmed(false);
    m_pendingCount = 0;
    for (PendingClick& click : m_pending) click = PendingClick{};
}

// ---------------------------------------------------------------------------
// Hook entry points
// ---------------------------------------------------------------------------

void GameSession::preEngineTick(PlayLayer* layer) {
    if (layer == nullptr || m_layer != layer) return;

    const PlayerObject* player = layer->m_player1;

    // Player identity change is the attempt boundary: first spawn, respawn after
    // death, and every practice-mode retry all produce a new object.
    if (player != m_lastPlayer) {
        if (player == nullptr) endAttempt();
        else beginAttempt();
        m_lastPlayer = player;
    }

    if (!m_attemptActive) return;

    // Arm delivery only when the engine is genuinely simulating. The tick index
    // advances either way, so a pause cannot desynchronise the macro.
    const bool armable = layer->m_started && !layer->m_isPaused &&
                         player != nullptr && !player->m_isDead;
    m_playback.setInjectionArmed(armable);

    const std::uint64_t before = m_playback.deliveredInputs();
    m_playback.preTick(layer, m_tick);

    // Whatever playback just injected is a click worth analysing.
    if (m_playback.deliveredInputs() != before) {
        queueClick(layer, m_tick, m_playback.speed());
    }
}

void GameSession::postEngineTick(PlayLayer* layer) {
    if (layer == nullptr || m_layer != layer || !m_attemptActive) return;

    m_tracker.sample(layer->m_player1, m_tick);
    if (m_settings.recorder) m_recorder.onTick();

    drainPendingClicks(layer);

    ++m_tick;
}

void GameSession::postFrame(PlayLayer* layer, float dt) {
    if (layer == nullptr || m_layer != layer) return;

    // Drain the alert latch first and unconditionally: an ingestion failure or a
    // playback desync must be reported even when the attempt is not active or the
    // overlay is off.
    reportPlaybackDesync();
    drainAlert();

    if (!m_attemptActive) return;
    if (!m_settings.overlay) return;

    m_overlay.beginFrame(dt);

    // One dot per boundary that actually happened since the last rendered frame,
    // at the position the player occupied on that tick. Accelerated mode can push
    // several boundaries inside a single frame, so this drains a list rather than
    // sampling the newest one.
    if (m_settings.boundaryDots) {
        const int found = m_tracker.boundariesAfter(
            m_lastDrawnBoundary, m_boundaryScratch.data(), Overlay::kMaxBoundaryDots);
        for (int i = 0; i < found; ++i) {
            m_overlay.addBoundaryMarker(m_boundaryScratch[i].x, m_boundaryScratch[i].y);
        }
    }

    // Advance the cursor whether or not the setting is on, and whether or not
    // anything was drawn. Advancing when nothing was drawn is safe because
    // `found == 0` means no boundary newer than the cursor exists, so there is
    // nothing to skip. Advancing while the setting is off stops a whole ring of
    // stale dots from being dumped on screen the moment it is switched back on.
    //
    // m_tick has already been incremented past the last simulated tick, so the
    // last simulated index is m_tick - 1.
    m_lastDrawnBoundary = m_tick > 0 ? m_tick - 1 : 0;

    if (m_stats.dirty()) {
        m_stats.clearDirty();
        m_stats.format(m_statsBuffer);
    }
    m_overlay.flush(m_statsBuffer);
}

void GameSession::onRawButton(PlayLayer* layer, bool down, int button, bool isPlayer1) {
    if (layer == nullptr || m_layer != layer || !m_attemptActive) return;
    if (button != kJumpButton) return;

    // Never record a replay as a fresh recording.
    if (m_playback.isRunning()) return;

    if (m_settings.recorder && m_recorder.isRecording()) {
        m_recorder.onButton(down, !isPlayer1);
    }

    queueClick(layer, m_tick, m_playback.speed());
}

// ---------------------------------------------------------------------------
// Click analysis
// ---------------------------------------------------------------------------

void GameSession::queueClick(PlayLayer* layer, std::uint64_t tick,
                             PlaybackSpeed speedAtInput) noexcept {
    if (m_pendingCount >= kMaxPendingClicks) return; // pool full; drop, never grow

    // Resolve the window immediately. The tracker already holds every boundary up
    // to and including the current tick, so deferring this would only risk using
    // a different answer once the next tick's boundary arrives.
    const InteractionTracker::BoundaryHit hit =
        m_tracker.nearestBoundaryHit(tick, kWindowSearchRadius);

    PendingClick& click = m_pending[static_cast<std::size_t>(m_pendingCount++)];
    click.valid = true;
    click.tick = tick;
    click.hasBoundary = hit.found;
    click.boundaryIsGround = hit.ground;
    click.speedAtInput = speedAtInput;

    if (hit.found && hit.tick >= 0) {
        const std::uint64_t boundary = static_cast<std::uint64_t>(hit.tick);
        const std::uint64_t distance = boundary > tick ? boundary - tick : tick - boundary;
        click.rawFrames =
            distance > static_cast<std::uint64_t>(kMaxWindowFrames)
                ? kMaxWindowFrames + 1
                : static_cast<int>(distance);
    } else {
        click.rawFrames = kMaxWindowFrames + 1; // "no boundary known" collapses to 1
    }

    // Capture the position at the input event itself. Reconstructing it later
    // would be wrong: by the time this is drained the player has moved.
    if (layer != nullptr && layer->m_player1 != nullptr) {
        click.worldX = layer->m_player1->m_position.x;
        click.worldY = layer->m_player1->m_position.y;
    } else {
        click.worldX = 0.f;
        click.worldY = 0.f;
    }
}

WindowContext GameSession::buildContext(PlayLayer* layer, const PendingClick& click) const noexcept {
    WindowContext ctx;

    ctx.allowSixtyOverride = m_settings.sixtyOverride;
    ctx.speed = click.speedAtInput;
    ctx.tick = click.tick;
    ctx.boundaryIsGround = click.hasBoundary && click.boundaryIsGround;

    if (layer != nullptr) {
        ctx.timeWarp = layer->m_gameState.m_timeWarp;
        ctx.playing = layer->m_started && !layer->m_isPaused;

        if (const PlayerObject* player = layer->m_player1) {
            ctx.playerAlive = !player->m_isDead;
            ctx.grounded = player->m_isOnGround || player->m_isOnGround2 ||
                           player->m_isOnGround3 || player->m_isOnGround4;
            ctx.onSlope = player->m_isCollidingWithSlope;
        }
    }

    return ctx;
}

void GameSession::drainPendingClicks(PlayLayer* layer) noexcept {
    if (m_pendingCount <= 0) return;

    for (int i = 0; i < m_pendingCount; ++i) {
        const PendingClick& click = m_pending[static_cast<std::size_t>(i)];
        if (!click.valid) continue;

        const WindowContext ctx = buildContext(layer, click);
        const WindowVerdict verdict = WindowAnalyzer::evaluate(click.rawFrames, ctx);

        m_stats.record(verdict);
        m_clickLog.append(ctx, verdict);

        if (m_settings.overlay) {
            m_overlay.addMarker(click.worldX, click.worldY, verdict.label.c_str(), verdict.overridden);
        }
    }

    m_pendingCount = 0;
    for (PendingClick& click : m_pending) click = PendingClick{};
}

// ---------------------------------------------------------------------------
// Macro ingestion
// ---------------------------------------------------------------------------

void GameSession::queueAlert(std::string text) {
    // Alerts are queued, not shown on the spot. Ingestion can happen from
    // PlayLayer::init, where the scene is not fully live yet, and an alert that
    // pops up on every practice-mode retry would be hostile. postFrame drains
    // this exactly once, when the scene is up.
    if (m_alertShown) return;
    m_alertText = std::move(text);
    m_alertShown = true;
}

void GameSession::drainAlert() {
    if (!m_alertShown || m_alertText.empty()) return;

    FLAlertLayer::create("AutoFPCount", m_alertText, "OK")->show();
    m_alertText.clear();
    m_alertShown = false;
}

void GameSession::reportPlaybackDesync() {
    if (m_desyncReported || !m_playback.desynced()) return;
    m_desyncReported = true;

    // A latched desync means the macro stopped rather than skipping a scheduled
    // step. That is the correct failure mode, but it is invisible unless the
    // player is told, and a macro that silently stops mid-run looks like the
    // mod hanging. So it is always reported, exactly once per run.
    const char* reason = m_playback.desyncReason();
    const std::string text =
        std::string("Macro playback stopped to stay frame-accurate: ") +
        (reason != nullptr ? reason : "unspecified reason") +
        "\n\nPlayback refuses to skip a recorded step, so it halted instead. "
        "The attempt itself is unaffected.";

    log::error("AutoFPCount playback desync: {}", reason != nullptr ? reason : "");
    queueAlert(text);
}

bool GameSession::importMacroData(MacroData&& data) {
    if (m_settings.rejectNon240) {
        const ValidationResult check = enforceNativeFps(data);
        if (!check.ok) {
            m_diagnostics = check.message;
            log::error("AutoFPCount rejected a macro: {}", check.message);
            return false;
        }
    }

    m_diagnostics = std::string(formatName(data.format)) + " / " +
                    std::to_string(data.inputs.size()) + " inputs / " +
                    std::to_string(data.declaredFps) + " FPS";
    if (!data.diagnostics.empty()) m_diagnostics += " (" + data.diagnostics + ")";
    if (!m_settings.rejectNon240) {
        m_diagnostics += " [240 FPS GATE DISABLED - this macro's tick rate is unverified]";
    }

    log::info("AutoFPCount ingested {}", m_diagnostics);

    m_playback.load(std::move(data));

    // A macro can land after the attempt has already begun - autoLoadFromDisk
    // runs during attach, but a live import can happen at any time. Start it now
    // rather than waiting for the next respawn.
    if (m_attemptActive && m_settings.autoStart) startPlayback();

    return true;
}

bool GameSession::importMacroFile(const std::filesystem::path& path) {
    MacroParseResult parsed = parseMacroFile(path);

    if (!parsed.ok) {
        const std::string reason = path.filename().string() + "\n\n" + parsed.message;
        m_diagnostics = reason;
        log::error("AutoFPCount rejected '{}': {}", path.filename().string(), parsed.message);
        queueAlert("Rejected " + path.filename().string() + ":\n" + parsed.message);
        return false;
    }

    if (!importMacroData(std::move(parsed.data))) {
        queueAlert("Rejected " + path.filename().string() + ":\n" + m_diagnostics);
        return false;
    }

    return true;
}

std::filesystem::path GameSession::autoLoadFromDisk() {
    // Mod::get() is null only if this somehow runs before the mod finished
    // loading. "." never throws, unlike temp_directory_path().
    Mod* mod = Mod::get();
    if (mod == nullptr) return {};
    const std::filesystem::path directory = mod->getSaveDir() / kMacroFolder;

    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) return {};

    // Deterministic ordering so the same file is chosen every launch.
    //
    // The extension list covers every writer in this build, not just the text
    // ones: a Silicate .slc or Eclipse .csv recording saved by this mod has to
    // be loadable again, and detectFormat() will still identify the format from
    // the file's contents rather than trusting the extension.
    static constexpr const char* kAcceptedExtensions[] = {
        ".macro", ".json", ".txt", ".slc", ".csv", ".gdr", ".gdr2",
    };

    std::vector<std::filesystem::path> candidates;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (!entry.is_regular_file(ec)) continue;

        std::string extension = entry.path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                       });

        bool accepted = false;
        for (const char* candidate : kAcceptedExtensions) {
            if (extension == candidate) {
                accepted = true;
                break;
            }
        }
        if (accepted) candidates.push_back(entry.path());
    }
    if (candidates.empty()) return {};

    std::sort(candidates.begin(), candidates.end());

    // Prefer the conventional filename, then fall back to the first candidate
    // that survives the gate.
    const auto preferred = directory / kActiveMacroName;
    if (std::find(candidates.begin(), candidates.end(), preferred) != candidates.end()) {
        if (importMacroFile(preferred)) return preferred;
        return {}; // an explicitly named active macro that fails must not silently
                   // fall through to a different file
    }

    for (const auto& candidate : candidates) {
        if (importMacroFile(candidate)) return candidate;
    }
    return {};
}

// ---------------------------------------------------------------------------
// Playback control
// ---------------------------------------------------------------------------

void GameSession::startPlayback() {
    if (!m_playback.isLoaded()) return;
    m_desyncReported = false;
    m_playback.restart();
    // Same rule as beginAttempt: the preset the user picked on the speed grid wins
    // over the mod.json toggle, so starting playback does not silently revert a
    // 73x choice back to the 4x default.
    m_playback.setSpeed(m_playback.rate() > 1.0 ? PlaybackSpeed::Accelerated
                                                : PlaybackSpeed::Normal);
    m_playback.start();
}

void GameSession::stopPlayback() {
    m_playback.setSpeed(PlaybackSpeed::Off);
}

void GameSession::restartPlayback() {
    startPlayback();
}

void GameSession::setPlaybackSpeed(PlaybackSpeed speed) {
    m_playback.setSpeed(speed);
    // A speed set from anywhere other than the preset grid invalidates the
    // preset selection, so the UI does not keep highlighting a button that no
    // longer describes what playback is doing.
    m_speedPreset = -1;
}

void GameSession::setSpeedPreset(int index) {
    if (index < 0 || index >= kSpeedPresetCount) return;

    const double rate = kSpeedPresets[index];
    m_speedPreset = index;

    m_playback.setRate(rate);
    // 1.0x is the vanilla clock and is expressed as Normal, which is also what
    // the 60 FPS override keys off: it refuses anything under acceleration
    // because the driven cadence is not the vanilla one.
    m_playback.setSpeed(rate > 1.0 ? PlaybackSpeed::Accelerated : PlaybackSpeed::Normal);
}

void GameSession::setClickOffset(int ticks) {
    m_playback.setClickOffset(ticks);
}

// ---------------------------------------------------------------------------
// Click log + window overrides
// ---------------------------------------------------------------------------

void GameSession::rebuildStatsFromClicks() {
    // Recomputed from the log rather than decremented/incremented per override.
    // The log is bounded at kCapacity entries and this only runs on a UI action,
    // so a full rebuild is a few thousand integer adds - cheaper than the class of
    // bug where a decrement is skipped and the tallies drift away from the list.
    m_stats.reset();
    for (int ordinal = 0; ordinal < m_clickLog.size(); ++ordinal) {
        if (const ClickRecord* record = m_clickLog.atOrdinal(ordinal)) {
            m_stats.record(record->effective);
        }
    }
}

void GameSession::setClickWindowOverride(std::uint64_t sequence, int frames) {
    if (m_clickLog.setOverride(sequence, frames) == nullptr) return;
    rebuildStatsFromClicks();
}

void GameSession::clearClickWindowOverride(std::uint64_t sequence) {
    if (m_clickLog.clearOverride(sequence) == nullptr) return;
    rebuildStatsFromClicks();
}

// ---------------------------------------------------------------------------
// Recording export
// ---------------------------------------------------------------------------

bool GameSession::exportRecording(const std::filesystem::path& path, MacroFormat format,
                                  std::string* errorMessage) {
    if (m_recorder.size() == 0) {
        if (errorMessage != nullptr) *errorMessage = "nothing has been recorded yet";
        return false;
    }
    return m_recorder.writeTo(path, format, errorMessage);
}

} // namespace afpc