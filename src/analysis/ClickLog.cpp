#include "ClickLog.hpp"

namespace afpc {

void ClickLog::reset() noexcept {
    // Only the cursors are cleared. The ring is left populated so that the
    // std::string members inside WindowVerdict keep their capacity - an attempt
    // restart should not churn through 1024 of them. append() overwrites every
    // field it cares about and sets valid = true, so stale slots are
    // unreachable rather than merely ignored.
    m_head = 0;
    m_count = 0;
    m_nextSequence = 1;
    m_overridden = 0;
}

void ClickLog::append(const WindowContext& context, const WindowVerdict& analyzed) noexcept {
    auto& record = m_records[m_head];

    record.sequence = m_nextSequence++;
    record.context = context;
    record.analyzed = analyzed;
    record.effective = analyzed;
    record.hasOverride = false;
    record.valid = true;

    m_head = (m_head + 1) % kCapacity;
    if (m_count < kCapacity) {
        ++m_count;
    }
}

int ClickLog::slotForOrdinal(int ordinal) const noexcept {
    // Negative ordinals count backwards from the newest, so -1 is the oldest
    // click still held in the ring.
    if (ordinal < 0) ordinal += m_count;
    if (ordinal < 0 || ordinal >= m_count) return -1;
    return (m_head - 1 - ordinal + kCapacity * 2) % kCapacity;
}

const ClickRecord* ClickLog::find(std::uint64_t sequence) const noexcept {
    const int slot = slotForSequence(sequence);
    return slot < 0 ? nullptr : &m_records[slot];
}

ClickRecord* ClickLog::findMutable(std::uint64_t sequence) noexcept {
    const int slot = slotForSequence(sequence);
    return slot < 0 ? nullptr : &m_records[slot];
}

int ClickLog::slotForSequence(std::uint64_t sequence) const noexcept {
    if (sequence == ClickRecord::kNoSequence) return -1;

    // Scans newest first. This is O(n) but only ever runs on a UI action - a
    // button press or a text commit - never on the click path, which uses
    // append() and is O(1).
    for (int ordinal = 0; ordinal < m_count; ++ordinal) {
        const int slot = slotForOrdinal(ordinal);
        if (slot < 0) break;
        if (m_records[slot].valid && m_records[slot].sequence == sequence) {
            return slot;
        }
    }
    return -1;
}

const ClickRecord* ClickLog::atOrdinal(int ordinal) const noexcept {
    const int slot = slotForOrdinal(ordinal);
    return slot < 0 ? nullptr : &m_records[slot];
}

ClickRecord* ClickLog::setOverride(std::uint64_t sequence, int frames) noexcept {
    auto* record = findMutable(sequence);
    if (record == nullptr) return nullptr;

    // Clamped exactly the way the tracker clamps its own raw window. Without
    // this a typed "0" would reach ceilingFpsForFrames and return a 0 FPS
    // label, and a negative would be meaningless.
    if (frames < 1) frames = 1;
    if (frames > kMaxWindowFrames) frames = kMaxWindowFrames;

    // Re-evaluated against the context captured at the click, not against the
    // player right now, so the 60 FPS override still sees the tick, the time
    // warp and the boundary kind that were true at the moment of the press.
    record->effective = WindowAnalyzer::evaluate(frames, record->context);

    if (!record->hasOverride) {
        ++m_overridden;
    }
    record->hasOverride = true;
    return record;
}

ClickRecord* ClickLog::clearOverride(std::uint64_t sequence) noexcept {
    auto* record = findMutable(sequence);
    if (record == nullptr) return nullptr;

    record->effective = record->analyzed;

    if (record->hasOverride) {
        --m_overridden;
    }
    record->hasOverride = false;
    return record;
}

} // namespace afpc
