#include "FrameStats.hpp"

namespace afpc {

void FrameStats::reset() noexcept {
    m_counts.fill(0);
    m_total = 0;
    m_overridden = 0;
    m_dirty = true;
}

void FrameStats::record(const WindowVerdict& verdict) noexcept {
    const auto index = static_cast<std::size_t>(verdict.bucket);
    if (index >= m_counts.size()) return; // unreachable via bucketFor

    ++m_counts[index];
    ++m_total;
    if (verdict.overridden) ++m_overridden;
    m_dirty = true;
}

void FrameStats::format(std::string& out) const {
    // 8 lines, longest label "11-12: " + up to 10 digits + newline ~= 17 bytes.
    out.clear();
    out.reserve(160);

    for (int i = 0; i < kBucketCount; ++i) {
        const auto bucket = static_cast<WindowBucket>(i);

        const char* label = bucketLabel(bucket);
        while (*label != '\0') out.push_back(*label++);
        out.push_back(':');
        out.push_back(' ');

        // Manual integer to text: this runs whenever a click lands, and keeping
        // it allocation-free means the tally update can never be the thing that
        // perturbs a frame.
        const std::uint32_t value = m_counts[static_cast<std::size_t>(i)];
        if (value == 0) {
            out.push_back('0');
        } else {
            char digits[12];
            int length = 0;
            std::uint32_t remaining = value;
            while (remaining > 0 && length < 12) {
                digits[length++] = static_cast<char>('0' + (remaining % 10u));
                remaining /= 10u;
            }
            while (length > 0) out.push_back(digits[--length]);
        }

        out.push_back('\n');
    }
}

} // namespace afpc