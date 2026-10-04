#pragma once

#include <array>
#include <cstdint>

#include "../core/Constants.hpp"
#include "WindowAnalyzer.hpp"

namespace afpc {

// ---------------------------------------------------------------------------
// ClickRecord
//
// One classified click. Two verdicts are kept deliberately:
//
//   analyzed  - what the InteractionTracker derived. Never mutated. This is the
//               measurement, and losing it would mean an override silently
//               destroyed the evidence it is correcting.
//   effective - what the tally counts. Equal to analyzed unless the user has
//               overridden this click.
//
// The original WindowContext is retained so an override can be re-evaluated
// against the state that was actually live at the click, rather than against
// whatever the player happens to be doing when they type the correction.
// ---------------------------------------------------------------------------
struct ClickRecord {
    static constexpr std::uint64_t kNoSequence = ~std::uint64_t{0};

    // Monotonic within an attempt and 1-based, so display numbering survives the
    // ring wrapping: click 700 still reads "#700" after 700 earlier clicks have
    // been overwritten.
    std::uint64_t sequence = kNoSequence;

    WindowContext context{};
    WindowVerdict analyzed{};
    WindowVerdict effective{};

    bool hasOverride = false;
    bool valid = false;
};

// ---------------------------------------------------------------------------
// ClickLog
//
// The per-attempt record of every click the mod classified. This is what the
// FRAMES list renders; before it existed, FrameStats kept only eight aggregate
// counters and a click was discarded the instant it had been counted.
//
// Storage is a fixed ring, allocated once. Nothing here allocates after
// construction and there are no locks: the log is only ever touched from the
// game's main thread, between ticks.
// ---------------------------------------------------------------------------
class ClickLog {
public:
    // 1024 clicks is far past any realistic manual attempt, and a full ring is a
    // deliberate, documented drop of the oldest entries rather than a realloc.
    static constexpr int kCapacity = 1024;

    void reset() noexcept;

    // Classifies and appends one click.
    void append(const WindowContext& context, const WindowVerdict& analyzed) noexcept;

    [[nodiscard]] int size() const noexcept { return m_count; }
    [[nodiscard]] bool empty() const noexcept { return m_count == 0; }

    // The sequence the next appended click will receive. Used by the caller to
    // correlate a click it just queued with the record the log produced.
    [[nodiscard]] std::uint64_t nextSequence() const noexcept { return m_nextSequence; }

    // Lookup by sequence, or nullptr if that click has aged out of the ring.
    [[nodiscard]] const ClickRecord* find(std::uint64_t sequence) const noexcept;

    // Newest-first ordinal access: ordinal 0 is the most recent click. Negative
    // ordinals count backwards from the newest. Returns nullptr when out of range.
    [[nodiscard]] const ClickRecord* atOrdinal(int ordinal) const noexcept;

    // Applies a user override. `frames` is clamped to [1, kMaxWindowFrames] the
    // same way the tracker clamps its own raw window, so a typed "0" or "-3"
    // becomes the tightest window instead of corrupting the divide by zero.
    // Returns the mutated record, or nullptr if the sequence is unknown.
    ClickRecord* setOverride(std::uint64_t sequence, int frames) noexcept;

    // Drops the override so the tracked measurement applies again. Returns the
    // mutated record, or nullptr if the sequence is unknown.
    ClickRecord* clearOverride(std::uint64_t sequence) noexcept;

    [[nodiscard]] std::size_t overriddenCount() const noexcept { return m_overridden; }

private:
    [[nodiscard]] int slotForOrdinal(int ordinal) const noexcept;
    [[nodiscard]] int slotForSequence(std::uint64_t sequence) const noexcept;

    ClickRecord* findMutable(std::uint64_t sequence) noexcept;

    std::array<ClickRecord, kCapacity> m_records{};
    int m_head = 0;   // next write slot
    int m_count = 0;  // valid entries, saturating at kCapacity
    std::uint64_t m_nextSequence = 1;
    std::size_t m_overridden = 0;
};

} // namespace afpc
