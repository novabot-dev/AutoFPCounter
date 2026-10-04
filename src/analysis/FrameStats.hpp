#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "WindowAnalyzer.hpp"

namespace afpc {

// ---------------------------------------------------------------------------
// FrameStats - the live "Frame Window: Count" tally.
//
// Counters are incremented the instant a click is classified, so the list is
// already current when the next frame renders it. No deferred aggregation, no
// background pass, no locks.
// ---------------------------------------------------------------------------
class FrameStats {
public:
    void reset() noexcept;

    void record(const WindowVerdict& verdict) noexcept;

    [[nodiscard]] std::uint32_t count(WindowBucket bucket) const noexcept {
        return m_counts[static_cast<std::size_t>(bucket)];
    }

    [[nodiscard]] std::uint64_t total() const noexcept { return m_total; }
    [[nodiscard]] std::uint64_t overriddenTotal() const noexcept { return m_overridden; }

    // True when at least one bucket changed since the last clearDirty().
    [[nodiscard]] bool dirty() const noexcept { return m_dirty; }
    void clearDirty() noexcept { m_dirty = false; }

    // Writes exactly:
    //
    //   1: 0
    //   2: 3
    //   3: 12
    //   4: 40
    //   5-6: 6
    //   7-8: 1
    //   9-10: 0
    //   11-12: 0
    //
    // into `out`, which is reused across calls so a steady state costs zero
    // allocations.
    void format(std::string& out) const;

private:
    std::array<std::uint32_t, kBucketCount> m_counts{};
    std::uint64_t m_total = 0;
    std::uint64_t m_overridden = 0;
    bool m_dirty = false;
};

} // namespace afpc