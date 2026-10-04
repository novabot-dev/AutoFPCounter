#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "../macro/MacroFormat.hpp"

namespace afpc {

// ---------------------------------------------------------------------------
// Recorder
//
// Captures raw player presses and releases as native 240 FPS structures.
//
// The capture point is GJBaseGameLayer::handleButton, which is the single point
// every input source funnels through - keyboard, touch, and the macro systems
// alike. Hooking it means the recorder sees exactly the events the engine
// itself acts on, with the engine's own button filtering already applied, and it
// costs one predicated branch per input event rather than a poll.
//
// Gating is the caller's job (see GameSession::onRawButton): the recorder is
// told whether the attempt is live, and only then does it accept events. During
// playback the recorder is suppressed so a replay is never re-recorded.
// ---------------------------------------------------------------------------
class Recorder {
public:
    // Starts a fresh capture. Safe to call every attempt; the event buffer's
    // capacity is retained so a retry performs zero allocations.
    void begin() noexcept;

    void end() noexcept;

    // Advances the capture frame index. Called once per engine tick.
    void onTick() noexcept;

    // Records one raw input. Ignored when no capture is active.
    void onButton(bool down, bool player2) noexcept;

    [[nodiscard]] bool isRecording() const noexcept { return m_recording; }
    [[nodiscard]] std::uint64_t tick() const noexcept { return m_tick; }
    [[nodiscard]] std::size_t size() const noexcept { return m_events.size(); }
    [[nodiscard]] const std::vector<MacroInput>& events() const noexcept { return m_events; }

    // Builds a MacroData view of the capture. `declaredFps` is always the native
    // 240 baseline: a recording produced by this recorder is by construction at
    // 240 FPS, so it satisfies the ingestion gate it would have to pass on the
    // way back in. Allocates, so it is not noexcept.
    [[nodiscard]] MacroData snapshot(MacroFormat format = MacroFormat::Unknown) const;

    // Serialises the capture. Returns false for formats with no writer.
    [[nodiscard]] bool exportAs(MacroFormat format, std::vector<std::uint8_t>& out) const;

    // Writes the capture to `path`.
    //
    // `format` may be MacroFormat::Unknown, in which case the writer is inferred
    // from the file extension (see formatForExtension). Pass a concrete format
    // when the extension is ambiguous or wrong - notably ".macro", which Silicate
    // uses for its binary container and which other tools use for text.
    [[nodiscard]] bool writeTo(const std::filesystem::path& path,
                               MacroFormat format = MacroFormat::Unknown,
                               std::string* errorMessage = nullptr) const;

    // Extension to writer mapping used when `format` is Unknown.
    [[nodiscard]] static MacroFormat formatForExtension(const std::filesystem::path& path) noexcept;

    // Sensible default output path inside the mod's save directory.
    [[nodiscard]] static std::filesystem::path defaultExportPath();

private:
    std::vector<MacroInput> m_events;
    std::uint64_t m_tick = 0;
    bool m_recording = false;
};

} // namespace afpc