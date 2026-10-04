#include "InteractionTracker.hpp"

#include <cmath>
#include <limits>

#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/PlayerObject.hpp>

namespace afpc {

namespace {

// Ring contact counts are only compared for equality, so anything past this
// saturates instead of risking an overflow on a malformed state.
constexpr int kMaxRingContacts = 64;

int objectIdOf(const GameObject* object) noexcept {
    return object != nullptr ? object->m_objectID : -1;
}

double gravityOrOne(double g) noexcept {
    return std::isfinite(g) ? g : 1.0;
}

// Ring slot holding the entry `index` counting back from the newest, which is
// where index 0 lives. The 2x capacity term keeps the modulo non-negative.
std::size_t ringSlot(int head, int index) noexcept {
    const int base = head - 1 - index + 2 * kBoundaryRingCapacity;
    return static_cast<std::size_t>(base % kBoundaryRingCapacity);
}

// Which of the sampled signals moved, so the window analysis can tell a
// ground-anchored boundary from an orb or input-state one.
enum class ChangeKind : std::uint8_t {
    None = 0,
    Ground,
    Other,
};

ChangeKind classifyChange(const InteractionSnapshot& a, const InteractionSnapshot& b) noexcept {
    // Ground first: a tick where the player left the floor, crossed onto a
    // different surface, or started/stopped sliding is the case where surface
    // geometry - and therefore a surface-anchored cadence - decides the outcome.
    //
    // Death is deliberately NOT in this group. A death tick is an input-state
    // transition, not evidence that a 60 Hz cadence would have decided the
    // landing differently, so tagging it "ground" would let the override fire on
    // the one tick where its premise is weakest.
    const bool groundMoved = a.ground1 != b.ground1 ||
                             a.ground2 != b.ground2 ||
                             a.ground3 != b.ground3 ||
                             a.ground4 != b.ground4 ||
                             a.onSlope != b.onSlope ||
                             a.sliding != b.sliding ||
                             a.groundObjectId != b.groundObjectId ||
                             a.slopeObjectId != b.slopeObjectId;
    if (groundMoved) return ChangeKind::Ground;

    const bool otherMoved = a.rotating != b.rotating ||
                            a.dead != b.dead ||
                            a.jumpBuffered != b.jumpBuffered ||
                            a.ringContacts != b.ringContacts ||
                            std::fabs(a.gravity - b.gravity) > 1e-9;
    if (otherMoved) return ChangeKind::Other;

    return ChangeKind::None;
}

} // namespace

void InteractionTracker::reset() noexcept {
    m_head = 0;
    m_count = 0;
    m_previous = InteractionSnapshot{};
    m_hasPrevious = false;
}

void InteractionTracker::push(std::uint64_t tick, float x, float y, bool groundRelated) noexcept {
    const std::size_t slot = static_cast<std::size_t>(m_head);
    m_ring[slot] = tick;
    m_xRing[slot] = x;
    m_yRing[slot] = y;
    m_tagRing[slot] = groundRelated ? 1u : 0u;
    m_head = (m_head + 1) % kBoundaryRingCapacity;
    if (m_count < kBoundaryRingCapacity) ++m_count;
}

void InteractionTracker::sample(const PlayerObject* player, std::uint64_t tick) noexcept {
    if (player == nullptr) {
        // No player means no meaningful baseline. Drop it so the next real
        // sample seeds the tracker instead of reporting one giant boundary.
        m_hasPrevious = false;
        return;
    }

    InteractionSnapshot current;
    current.tick = tick;
    current.x = player->m_position.x;
    current.y = player->m_position.y;
    current.ground1 = player->m_isOnGround;
    current.ground2 = player->m_isOnGround2;
    current.ground3 = player->m_isOnGround3;
    current.ground4 = player->m_isOnGround4;
    current.onSlope = player->m_isCollidingWithSlope;
    current.sliding = player->m_isSliding;
    current.rotating = player->m_isRotating;
    current.dead = player->m_isDead;
    current.jumpBuffered = player->m_jumpBuffered;
    current.groundObjectId = objectIdOf(player->m_lastGroundObject);
    current.slopeObjectId = objectIdOf(player->m_currentSlope);
    current.ringContacts =
        static_cast<int>(player->m_touchedRings.size() > static_cast<std::size_t>(kMaxRingContacts)
                             ? static_cast<std::size_t>(kMaxRingContacts)
                             : player->m_touchedRings.size());
    current.gravity = gravityOrOne(player->m_gravity);

    // The very first sample of an attempt is itself the origin of the window
    // space, so it is recorded as boundary tick 0.
    if (!m_hasPrevious) {
        m_hasPrevious = true;
        m_previous = current;
        push(tick, current.x, current.y, false);
        return;
    }

    const ChangeKind change = classifyChange(m_previous, current);
    m_previous = current;

    if (change != ChangeKind::None) {
        push(tick, current.x, current.y, change == ChangeKind::Ground);
    }
}

int InteractionTracker::boundariesAfter(std::uint64_t afterTick, BoundaryPoint* out,
                                        int maxOut) const noexcept {
    if (out == nullptr || maxOut <= 0 || m_count <= 0) return 0;

    // How many boundaries are newer than the caller's cursor. Entries were pushed
    // in ascending tick order and index 0 is the newest, so counting forward stops
    // at the first entry that is not newer.
    int total = 0;
    for (int i = 0; i < m_count; ++i) {
        const std::size_t slot = ringSlot(m_head, i);
        if (afterTick != kNoBoundaryCursor && m_ring[slot] <= afterTick) break;
        ++total;
    }
    if (total <= 0) return 0;

    // When more boundaries happened than the caller can draw, keep the most recent
    // ones - they are the ones on screen. Walking back from the newest and writing
    // from index 0 upwards leaves `out` in ascending age order.
    const int kept = total < maxOut ? total : maxOut;
    for (int i = 0; i < kept; ++i) {
        const std::size_t slot = ringSlot(m_head, total - 1 - i);
        out[i].x = m_xRing[slot];
        out[i].y = m_yRing[slot];
        out[i].ground = m_tagRing[slot] != 0u;
    }
    return kept;
}

InteractionTracker::BoundaryHit InteractionTracker::nearestBoundaryHit(std::uint64_t tick,
                                                                       int radius) const noexcept {
    BoundaryHit best;
    if (m_count <= 0) return best;
    if (radius < 0) radius = 0;

    const std::uint64_t span = static_cast<std::uint64_t>(radius);
    std::uint64_t bestDistance = std::numeric_limits<std::uint64_t>::max();

    // Entries were written in ascending tick order, so an entry at distance 0 is
    // a complete answer and the scan can stop there.
    for (int i = 0; i < m_count; ++i) {
        const std::size_t slot = ringSlot(m_head, i);
        const std::uint64_t entry = m_ring[slot];

        const std::uint64_t distance = entry > tick ? entry - tick : tick - entry;
        if (distance > span) continue;

        if (distance == 0) {
            best.found = true;
            best.tick = static_cast<std::int64_t>(entry);
            best.ground = m_tagRing[slot] != 0;
            return best;
        }
        if (distance < bestDistance) {
            bestDistance = distance;
            best.found = true;
            best.tick = static_cast<std::int64_t>(entry);
            best.ground = m_tagRing[slot] != 0;
        }
    }
    return best;
}

} // namespace afpc