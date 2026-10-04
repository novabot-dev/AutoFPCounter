#pragma once

#include <array>
#include <cstdint>

#include "../core/Constants.hpp"

// Geode's game bindings are declared in the GLOBAL namespace - geode::prelude
// deliberately does not pull them in, which is why every Geode mod can write
// `$modify(PlayLayer)` unqualified.
//
// This forward declaration therefore has to sit at global scope too. Declaring
// `class PlayerObject;` *inside* namespace afpc would introduce a second,
// incomplete PlayerObject that shadows the real binding for every unqualified use
// in this header, and `player->m_isOnGround` would then fail to compile against
// an incomplete type.
class PlayerObject;

namespace afpc {

// ---------------------------------------------------------------------------
// InteractionTracker
//
// WHAT THIS MEASURES, PRECISELY
// ------------------------------
// A click's "frame window" is the number of ticks of slack the player has
// before the press stops landing on the target surface. Deriving that exactly
// requires a full physics roll-forward (move the player N ticks with and
// without the click and see whether the outcome differs), which cannot be done
// cheaply during live gameplay.
//
// This tracker takes the standard cheap approximation used by practice tools
// and frame trainers: it records the ticks at which the player's *interaction
// state* changes. Those ticks are the only ticks where an extra or missing tick
// of input can change what the player collides with, so the distance from a
// click to the nearest such tick is a tight upper bound on the slack.
//
// Signals sampled once per tick (all reads are plain member loads on the game
// thread, no allocation, no virtual calls):
//   * four of the engine's ground flags
//   * slope contact, and which object the slope is
//   * the ground object id
//   * gravity direction
//   * ring/orb contact counts
//   * sliding, rotating, dead, jump buffered
//
// HONEST LIMITATION
// -----------------
// This is a proxy. It is a very good one - it changes exactly when the physics
// decision changes - but it cannot detect a window whose bounds come from an
// interaction this tracker does not sample. See docs/FORMATS.md.
// ---------------------------------------------------------------------------

struct InteractionSnapshot {
    std::uint64_t tick = 0;

    // Player world position on this tick, so a caller can place a marker without
    // re-reading the player object.
    float x = 0.f;
    float y = 0.f;

    bool ground1 = false;
    bool ground2 = false;
    bool ground3 = false;
    bool ground4 = false;
    bool onSlope = false;
    bool sliding = false;
    bool rotating = false;
    bool dead = false;
    bool jumpBuffered = false;
    int groundObjectId = -1;
    int slopeObjectId = -1;
    int ringContacts = 0;
    double gravity = 1.0;
};

class InteractionTracker {
public:
    void reset() noexcept;

    // Call once per logic tick, AFTER the engine's own update. Passing a null
    // player (level not started, or player removed) invalidates the previous
    // sample so the next real sample re-seeds instead of reporting a spurious
    // boundary.
    void sample(const PlayerObject* player, std::uint64_t tick) noexcept;

    // The nearest recorded boundary to `tick`, plus whether that boundary was a
    // ground/slope transition rather than an orb or input-state transition.
    struct BoundaryHit {
        bool found = false;
        std::int64_t tick = -1;
        bool ground = false;
    };

    // The single query the window analysis needs. `found` is false when no
    // boundary is within `radius`, in which case the caller treats the window as
    // unknown and collapses it to the tightest bucket rather than inventing one.
    [[nodiscard]] BoundaryHit nearestBoundaryHit(std::uint64_t tick,
                                                  int radius = kWindowSearchRadius) const noexcept;

    // A recorded boundary, with the player position on the tick it happened.
    struct BoundaryPoint {
        float x = 0.f;
        float y = 0.f;
        bool ground = false;
    };

    // Copies up to `maxOut` boundaries recorded strictly after `afterTick` into
    // `out`, oldest first, and returns how many were copied. Pass the previously
    // returned cursor as `afterTick` to walk forward exactly once per boundary.
    //
    // This exists so the overlay can draw one dot per *actual* boundary rather
    // than one per rendered frame: accelerated mode can push several boundaries
    // inside a single frame, and a per-frame dot would silently misreport them.
    // When more boundaries occurred than fit in `maxOut`, the most recent ones are
    // kept, because those are the ones still on screen.
    //
    // Pass kNoBoundaryCursor as `afterTick` to receive every boundary in the ring,
    // including the attempt's tick-0 origin.
    int boundariesAfter(std::uint64_t afterTick, BoundaryPoint* out, int maxOut) const noexcept;

    static constexpr std::uint64_t kNoBoundaryCursor = ~std::uint64_t{0};

private:
    void push(std::uint64_t tick, float x, float y, bool groundRelated) noexcept;

    // Parallel ring arrays. Storing the position alongside the tick is what lets
    // the overlay draw a dot at the exact spot the boundary happened without
    // re-reading the player object after it has already moved on.
    std::array<std::uint64_t, kBoundaryRingCapacity> m_ring{};
    std::array<float, kBoundaryRingCapacity> m_xRing{};
    std::array<float, kBoundaryRingCapacity> m_yRing{};
    std::array<std::uint8_t, kBoundaryRingCapacity> m_tagRing{};
    int m_head = 0;  // next write slot
    int m_count = 0; // valid entries, saturating at capacity

    // Only the previous sample is retained. The current one is compared against it
    // and then folded in, so there is no reason to keep a third copy around.
    InteractionSnapshot m_previous{};
    bool m_hasPrevious = false;
};

} // namespace afpc