#pragma once

#include <array>
#include <cstdint>
#include <string>

#include <Geode/cocos/draw_nodes/CCDrawNode.h>
#include <Geode/cocos/label_nodes/CCLabelBMFont.h>

#include "../core/Constants.hpp"

// PlayLayer is a Geode binding and lives in the GLOBAL namespace. See the note in
// InteractionTracker.hpp for why this declaration must not move inside `afpc`.
class PlayLayer;

// The cocos2d classes are the opposite case: they live in `namespace cocos2d`,
// not globally. Geode's own headers do not re-export them at global scope, so the
// exact names used below are pulled in explicitly rather than with a blanket
// `using namespace`, which would risk ambiguities against the Geode bindings.
using cocos2d::CCDirector;
using cocos2d::CCDrawNode;
using cocos2d::CCLabelBMFont;
using cocos2d::CCNode;
using cocos2d::CCPoint;
using cocos2d::CCSize;
using cocos2d::ccColor3B;
using cocos2d::ccColor4F;

namespace afpc {

// ---------------------------------------------------------------------------
// Overlay
//
// DRAW MODEL
// ----------
//   * ONE CCDrawNode for every circle, boundary dot and the statistics panel.
//     Clearing and repopulating one batched node per frame is dramatically
//     cheaper than one node per marker, and it keeps the draw-call count
//     constant regardless of how many markers are alive.
//
//   * A FIXED POOL of CCLabelBMFont for the per-marker frame-rate strings.
//     Labels are created lazily on first use and recycled for the rest of the
//     layer's life, so after warm-up no marker ever allocates.
//
//   * ONE CCLabelBMFont for the statistics list, whose string is only pushed
//     into the node when a counter actually changed (dirty check). A steady
//     state therefore performs zero text uploads per frame.
//
// FRAME ORDER (this is the whole point of the design)
// --------------------------------------------------
// Geometry Dash runs, per rendered frame:
//
//     update()      x N    -> clicks get classified, markers get QUEUED
//     postUpdate()       -> postFrame() -> beginFrame() then flush()
//
// beginFrame() runs AFTER markers have been queued, so it must not be the thing
// that draws them - it would clear the batch before anything rendered. So:
//   beginFrame()  ages and compacts the marker list, clears the draw nodes
//   addMarker()    pure data, touches no cocos node
//   flush()        draws everything for this frame, positions every label
//
// COORDINATES
// ----------
// Circles and marker labels live in *world* space so they travel with the
// camera, which is what makes them line up with the orb or platform the click
// belongs to. The statistics list lives in *screen* space: its position is
// recomputed each frame from the director's win size via
// CCNode::convertToNodeSpace, so it stays pinned to the corner while the world
// scrolls underneath it.
//
// LIFETIME
// --------
// All nodes are children of the PlayLayer. detach() removes them with cleanup
// and nulls every pointer, so the overlay can never touch a node after its
// layer is gone.
// ---------------------------------------------------------------------------
class Overlay {
public:
    // Live markers are pooled; this is a hard cap, not a hint.
    static constexpr int kMaxLiveMarkers = 48;

    // Interaction-boundary dots drawn per rendered frame.
    static constexpr int kMaxBoundaryDots = 64;

    bool attach(PlayLayer* layer);
    void detach();

    [[nodiscard]] bool attached() const noexcept { return m_layer != nullptr; }

    void setVisible(bool visible) noexcept;
    [[nodiscard]] bool visible() const noexcept { return m_visible; }

    // Clears the draw batch and ages the marker pool. Call once per rendered
    // frame, before anything new is queued.
    void beginFrame(float dt) noexcept;

    // Queues a marker. Pure data - no cocos node is touched here.
    void addMarker(float x, float y, const char* text, bool overridden) noexcept;

    // Queues an interaction-boundary dot. Pure data, like addMarker.
    void addBoundaryMarker(float x, float y) noexcept;

    // Draws every live marker, every queued boundary dot, the statistics panel
    // and the statistics text. Call once per rendered frame, after the engine's
    // postUpdate has finished.
    void flush(const std::string& statsText);

    // Drops every live marker. Used on attempt reset and when hiding the overlay.
    void clearMarkers() noexcept;

private:
    struct Marker {
        float x = 0.f;
        float y = 0.f;
        float remaining = 0.f;
        bool overridden = false;
        char text[16] = {0};
    };

    void buildNodes();
    void releaseNodes();
    CCLabelBMFont* acquireLabel(int index);
    void positionStatsLabel();

    PlayLayer* m_layer = nullptr;

    CCDrawNode* m_worldDraw = nullptr;  // circles + boundary dots, world space
    CCDrawNode* m_panelDraw = nullptr;  // statistics backdrop, world space
    CCLabelBMFont* m_statsLabel = nullptr;

    // Pool: one label per slot, created lazily and reused thereafter.
    std::array<CCLabelBMFont*, kMaxLiveMarkers> m_labelPool{};

    // Text currently uploaded to each pooled label. Compared before setString so a
    // live marker's string is pushed once rather than once per frame - setString
    // re-uploads the bitmap-font atlas, so a redundant call is not free.
    std::array<std::array<char, 16>, kMaxLiveMarkers> m_slotText{};
    std::array<bool, kMaxLiveMarkers> m_slotUsed{};

    std::array<Marker, kMaxLiveMarkers> m_markers{};
    int m_liveCount = 0;

    // Boundary dots queued for this rendered frame.
    std::array<float, kMaxBoundaryDots> m_dotX{};
    std::array<float, kMaxBoundaryDots> m_dotY{};
    int m_dotCount = 0;

    // Last statistics string actually uploaded to the label node. Comparing
    // against it keeps setString() off the steady-state path.
    std::string m_flushedStats;

    // Cached scaled size of the statistics label, recomputed only when the text
    // changed. CCNode has no getScaledWidth/getScaledHeight in this binding.
    float m_panelWidth = 0.f;
    float m_panelHeight = 0.f;
    bool m_panelSizeValid = false;

    bool m_visible = true;
};

} // namespace afpc