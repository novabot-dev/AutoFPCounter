#include "Overlay.hpp"

#include <cmath>
#include <cstring>

#include <Geode/binding/PlayLayer.hpp>
#include <Geode/cocos/CCDirector.h>

using namespace geode::prelude;

namespace afpc {

namespace {

// ---------------------------------------------------------------------------
// Draw-order tags. Gameplay nodes live far below this.
// ---------------------------------------------------------------------------
constexpr int kWorldDrawZ = 900000;
constexpr int kPanelDrawZ = 900001;
constexpr int kMarkerLabelZ = 900002;
constexpr int kStatsLabelZ = 900003;

// ---------------------------------------------------------------------------
// Marker geometry, in world units.
// ---------------------------------------------------------------------------
constexpr float kMarkerRadius = 7.f;
constexpr float kMarkerRingGap = 1.6f;
constexpr float kMarkerTextGap = 5.f;
constexpr float kMarkerTextOffset = kMarkerRadius + kMarkerRingGap + kMarkerTextGap;
constexpr float kBoundaryDotSize = 2.5f;

// The click "circle" is a filled disc plus an outline built from this many dots
// evenly spaced around the circumference. drawCircle() is deliberately NOT used:
// Geode binds it with six parameters - two ccColor4F values and an extra uint -
// whose meaning is not documented, so its argument order cannot be relied on.
// drawDot() has one unambiguous signature, is already the primary primitive, and
// a 24-dot ring is visually indistinguishable at this radius. All of it lands in
// one batched draw node, so the extra primitives cost a few vertices each.
constexpr int kRingDotCount = 24;
constexpr float kRingDotSize = 0.85f;

// ---------------------------------------------------------------------------
// Text.
//
// CCLabelBMFont's bound factories are create(str, fntFile, ...). There is no
// createWithFontAndText in this binding, so scale is applied separately with
// setScale(), which CCLabelBMFont does override.
// ---------------------------------------------------------------------------
constexpr char kStatsFont[] = "bigFont.fnt";
constexpr char kMarkerFont[] = "goldFont.fnt";
constexpr float kStatsScale = 0.42f;
constexpr float kMarkerScale = 0.5f;

constexpr float kStatsPad = 6.f;
constexpr float kStatsMargin = 12.f;

// Panel colours. `const` rather than `constexpr`: cocos2d's colour structs are
// aggregates with ordinary constructors, which are not constexpr, so a constexpr
// instance would not compile.
const ccColor4F kPanelFill{0.f, 0.f, 0.f, 0.55f};
const ccColor4F kPanelBorder{1.f, 1.f, 1.f, 0.18f};

// Dot around a circle. sin/cos per dot per frame is 48 dots * 24 = 1152 trig
// pairs worst case, which is irrelevant next to the vertex work, but the angles
// are constants rather than accumulated so there is no drift to accumulate.
void ringDot(float centreX, float centreY, float radius, float index, float& outX,
             float& outY) noexcept {
    const float angle = (kTwoPi / static_cast<float>(kRingDotCount)) * index;
    outX = centreX + radius * std::cos(angle);
    outY = centreY + radius * std::sin(angle);
}

ccColor3B rgb(unsigned char r, unsigned char g, unsigned char b) {
    ccColor3B color;
    color.r = r;
    color.g = g;
    color.b = b;
    return color;
}

// CCNode itself carries no opacity setter - setOpacity lives on CCNodeRGBA (the
// base of CCDrawNode) and is re-declared directly on CCLabelBMFont, which instead
// implements CCRGBAProtocol. There is no shared base that exposes it, so both
// concrete shapes are offered and overload resolution picks per call site.
void setNodeOpacity(cocos2d::CCNodeRGBA* node, unsigned char alpha) noexcept {
    if (node != nullptr) node->setOpacity(alpha);
}

void setNodeOpacity(cocos2d::CCLabelBMFont* node, unsigned char alpha) noexcept {
    if (node != nullptr) node->setOpacity(alpha);
}

// Marker alpha for the current point in its lifetime. Full opacity for the first
// 60% of the life, then a linear fade, so a marker visibly expires instead of
// popping out of existence.
float markerAlpha(float remaining) noexcept {
    const float life = static_cast<float>(kMarkerLifetime);
    if (!(life > 0.f)) return 1.f;

    const float fraction = remaining / life;
    if (fraction >= 0.4f) return 1.f;
    if (fraction <= 0.f) return 0.f;
    return fraction / 0.4f;
}

} // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool Overlay::attach(PlayLayer* layer) {
    if (layer == nullptr) return false;
    if (m_layer != nullptr) detach();

    m_layer = layer;
    buildNodes();

    if (m_worldDraw == nullptr || m_panelDraw == nullptr || m_statsLabel == nullptr) {
        // Node creation failed. Do not leave a half-built overlay behind: a
        // missing draw node would silently swallow every circle, which is far
        // worse than no overlay at all.
        releaseNodes();
        m_layer = nullptr;
        return false;
    }

    m_liveCount = 0;
    m_dotCount = 0;
    m_flushedStats.clear();
    m_panelSizeValid = false;
    m_slotUsed.fill(false);
    positionStatsLabel();
    return true;
}

void Overlay::detach() {
    releaseNodes();
    m_layer = nullptr;
    m_liveCount = 0;
    m_dotCount = 0;
    m_flushedStats.clear();
    m_panelSizeValid = false;
    // The label nodes are destroyed, so the cached "already uploaded" text no
    // longer describes anything and has to be invalidated.
    m_slotUsed.fill(false);
}

void Overlay::buildNodes() {
    PlayLayer* layer = m_layer;
    if (layer == nullptr) return;

    // No explicit blend function is set. CCDrawNode::init already selects
    // premultiplied alpha (GL_ONE, GL_ONE_MINUS_SRC_ALPHA) in cocos2d-x 2.x, which
    // is what translucent geometry wants, and relying on it avoids depending on the
    // GL constant headers at all.
    m_worldDraw = CCDrawNode::create();
    if (m_worldDraw != nullptr) {
        m_worldDraw->setZOrder(kWorldDrawZ);
        layer->addChild(m_worldDraw, kWorldDrawZ);
    }

    m_panelDraw = CCDrawNode::create();
    if (m_panelDraw != nullptr) {
        m_panelDraw->setZOrder(kPanelDrawZ);
        layer->addChild(m_panelDraw, kPanelDrawZ);
    }

    m_statsLabel = CCLabelBMFont::create("", kStatsFont);
    if (m_statsLabel != nullptr) {
        m_statsLabel->setScale(kStatsScale);
        m_statsLabel->setZOrder(kStatsLabelZ);
        m_statsLabel->setAnchorPoint(CCPoint(1.f, 1.f));
        m_statsLabel->setColor(rgb(255, 255, 255));
        layer->addChild(m_statsLabel, kStatsLabelZ);
    }
}

void Overlay::releaseNodes() {
    PlayLayer* layer = m_layer;

    for (CCLabelBMFont*& label : m_labelPool) {
        if (label == nullptr) continue;
        if (layer != nullptr) layer->removeChild(label, true);
        label = nullptr;
    }

    if (m_statsLabel != nullptr) {
        if (layer != nullptr) layer->removeChild(m_statsLabel, true);
        m_statsLabel = nullptr;
    }
    if (m_worldDraw != nullptr) {
        if (layer != nullptr) layer->removeChild(m_worldDraw, true);
        m_worldDraw = nullptr;
    }
    if (m_panelDraw != nullptr) {
        if (layer != nullptr) layer->removeChild(m_panelDraw, true);
        m_panelDraw = nullptr;
    }
}

void Overlay::setVisible(bool visible) noexcept {
    m_visible = visible;

    if (!visible) {
        clearMarkers();
        if (m_worldDraw != nullptr) m_worldDraw->clear();
        if (m_panelDraw != nullptr) m_panelDraw->clear();
        setNodeOpacity(m_worldDraw, 0);
        setNodeOpacity(m_panelDraw, 0);
        setNodeOpacity(m_statsLabel, 0);
        for (CCLabelBMFont* label : m_labelPool) setNodeOpacity(label, 0);
        return;
    }

    setNodeOpacity(m_worldDraw, 255);
    setNodeOpacity(m_panelDraw, 255);
    setNodeOpacity(m_statsLabel, 255);
}

void Overlay::clearMarkers() noexcept {
    m_liveCount = 0;
    m_dotCount = 0;
    for (Marker& marker : m_markers) marker.remaining = 0.f;
}

// ---------------------------------------------------------------------------
// Per-frame update
// ---------------------------------------------------------------------------

void Overlay::beginFrame(float dt) noexcept {
    if (m_worldDraw != nullptr) m_worldDraw->clear();
    if (m_panelDraw != nullptr) m_panelDraw->clear();

    m_dotCount = 0;

    if (!m_visible) return;

    // A non-finite or negative dt would make every marker immortal. Clamp the
    // step to the full lifetime, which simply drops the whole batch early.
    float step = 0.f;
    if (dt > 0.f) step = dt;
    if (!(step <= static_cast<float>(kMarkerLifetime))) step = static_cast<float>(kMarkerLifetime);

    if (step > 0.f) {
        // Age and compact in place, so a burst of clicks cannot leave holes in the
        // pool. O(live), no allocation.
        int write = 0;
        for (int read = 0; read < m_liveCount; ++read) {
            Marker& marker = m_markers[static_cast<std::size_t>(read)];
            marker.remaining -= step;
            if (marker.remaining <= 0.f) continue;
            if (write != read) m_markers[static_cast<std::size_t>(write)] = marker;
            ++write;
        }
        m_liveCount = write;
    }

    // Hide labels beyond the live count so a recycled slot cannot linger on
    // screen from a previous burst.
    for (int i = m_liveCount; i < kMaxLiveMarkers; ++i) {
        setNodeOpacity(m_labelPool[static_cast<std::size_t>(i)], 0);
    }
}

void Overlay::addMarker(float x, float y, const char* text, bool overridden) noexcept {
    if (!m_visible || text == nullptr) return;

    if (m_liveCount >= kMaxLiveMarkers) {
        // Pool exhausted. Drop the oldest rather than growing: a marker lives
        // kMarkerLifetime, so the oldest is by definition the least useful.
        for (int i = 1; i < m_liveCount; ++i) {
            m_markers[static_cast<std::size_t>(i - 1)] = m_markers[static_cast<std::size_t>(i)];
        }
        if (m_liveCount > 0) --m_liveCount;
    }

    Marker& marker = m_markers[static_cast<std::size_t>(m_liveCount)];
    marker.x = x;
    marker.y = y;
    marker.remaining = static_cast<float>(kMarkerLifetime);
    marker.overridden = overridden;

    std::memset(marker.text, 0, sizeof(marker.text));
    std::size_t i = 0;
    for (; text[i] != '\0' && i + 1 < sizeof(marker.text); ++i) {
        marker.text[i] = text[i];
    }

    ++m_liveCount;
}

void Overlay::addBoundaryMarker(float x, float y) noexcept {
    if (!m_visible || m_dotCount >= kMaxBoundaryDots) return;
    m_dotX[static_cast<std::size_t>(m_dotCount)] = x;
    m_dotY[static_cast<std::size_t>(m_dotCount)] = y;
    ++m_dotCount;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

CCLabelBMFont* Overlay::acquireLabel(int index) {
    if (index < 0 || index >= kMaxLiveMarkers) return nullptr;

    CCLabelBMFont*& slot = m_labelPool[static_cast<std::size_t>(index)];
    if (slot != nullptr) return slot;

    slot = CCLabelBMFont::create("", kMarkerFont);
    if (slot == nullptr) return nullptr;

    slot->setScale(kMarkerScale);
    slot->setZOrder(kMarkerLabelZ);
    slot->setAnchorPoint(CCPoint(0.f, 0.5f));
    slot->setColor(rgb(255, 255, 255));
    if (m_layer != nullptr) m_layer->addChild(slot, kMarkerLabelZ);
    return slot;
}

void Overlay::positionStatsLabel() {
    if (m_layer == nullptr || m_statsLabel == nullptr) return;

    const CCSize winSize = cocos2d::CCDirector::sharedDirector()->getWinSize();
    const CCPoint screenCorner(winSize.width - kStatsMargin, winSize.height - kStatsMargin);
    m_statsLabel->setPosition(m_layer->convertToNodeSpace(screenCorner));
}

void Overlay::flush(const std::string& statsText) {
    if (m_layer == nullptr) return;

    positionStatsLabel();

    // setString() re-uploads the bitmap-font atlas, so only do it when the tally
    // actually changed. A steady-state frame performs no text work at all.
    if (m_statsLabel != nullptr && statsText != m_flushedStats) {
        m_statsLabel->setString(statsText.c_str());
        m_flushedStats = statsText;
        // CCLabelBMFont::setString recomputes the content size, so the panel has
        // to be re-measured whenever the text changed.
        m_panelSizeValid = false;
    }

    if (m_worldDraw != nullptr) {
        // Interaction-boundary dots first, so click circles draw on top of them.
        for (int i = 0; i < m_dotCount; ++i) {
            m_worldDraw->drawDot(CCPoint(m_dotX[static_cast<std::size_t>(i)],
                                         m_dotY[static_cast<std::size_t>(i)]),
                                 kBoundaryDotSize, ccColor4F{1.f, 1.f, 1.f, 0.22f});
        }

        for (int i = 0; i < m_liveCount; ++i) {
            const Marker& marker = m_markers[static_cast<std::size_t>(i)];

            const float alpha = markerAlpha(marker.remaining);
            if (alpha <= 0.f) continue;

            const CCPoint position(marker.x, marker.y);

            // Overridden markers are green so the exception path is visually
            // obvious next to an ordinary cyan one.
            const ccColor4F fill = marker.overridden
                                       ? ccColor4F{0.15f, 1.f, 0.55f, 0.35f * alpha}
                                       : ccColor4F{0.15f, 0.85f, 1.f, 0.30f * alpha};
            m_worldDraw->drawDot(position, kMarkerRadius, fill);

            const ccColor4F ring{1.f, 1.f, 1.f, 0.85f * alpha};
            const float ringRadius = kMarkerRadius + kMarkerRingGap;
            for (int d = 0; d < kRingDotCount; ++d) {
                float dx = 0.f;
                float dy = 0.f;
                ringDot(marker.x, marker.y, ringRadius, static_cast<float>(d), dx, dy);
                m_worldDraw->drawDot(CCPoint(dx, dy), kRingDotSize, ring);
            }
        }
    }

    // Labels next to their circles.
    for (int i = 0; i < m_liveCount; ++i) {
        const Marker& marker = m_markers[static_cast<std::size_t>(i)];

        CCLabelBMFont* label = acquireLabel(i);
        if (label == nullptr) continue;

        label->setPosition(marker.x + kMarkerTextOffset, marker.y);
        label->setColor(marker.overridden ? rgb(120, 255, 140) : rgb(255, 255, 255));

        // setString() re-uploads the bitmap-font atlas, so a live marker's text is
        // pushed to its slot exactly once, not once per frame. The slot index is
        // stable for a marker's whole life (markers are compacted in place, never
        // reordered), and the comparison makes a recycled slot pick up whatever
        // text it now has to show.
        const std::size_t slot = static_cast<std::size_t>(i);
        if (!m_slotUsed[slot] ||
            std::memcmp(m_slotText[slot].data(), marker.text, sizeof(marker.text)) != 0) {
            label->setString(marker.text);
            std::memcpy(m_slotText[slot].data(), marker.text, sizeof(marker.text));
            m_slotUsed[slot] = true;
        }

        const auto opacity =
            static_cast<unsigned char>(255.f * markerAlpha(marker.remaining));
        setNodeOpacity(label, opacity);
    }

    // Panel behind the statistics list.
    if (m_panelDraw == nullptr || m_statsLabel == nullptr) return;

    if (!m_panelSizeValid) {
        // CCNode has no getScaledWidth/getScaledHeight in this binding, so the
        // scaled size is the content size multiplied by the node scale.
        const CCSize size = m_statsLabel->getContentSize();
        const float scale = m_statsLabel->getScale();
        m_panelWidth = size.width * scale;
        m_panelHeight = size.height * scale;
        m_panelSizeValid = true;
    }

    if (m_panelWidth <= 0.f || m_panelHeight <= 0.f) return;

    // Anchor is (1,1), so the label's position is its own top-right corner.
    const CCPoint topRight = m_statsLabel->getPosition();
    const CCPoint bottomLeft(topRight.x - m_panelWidth - kStatsPad,
                             topRight.y - m_panelHeight - kStatsPad);
    const CCPoint topRightPadded(topRight.x + kStatsPad, topRight.y + kStatsPad);

    // drawRect's final argument is a cocos2d::BorderAlignment. The enumerator names
    // are not published in the SDK docs, so value 0 - always a valid enumerator -
    // is used rather than guessing a symbol that may not exist.
    m_panelDraw->drawRect(bottomLeft, topRightPadded, kPanelFill, 1.f, kPanelBorder,
                          static_cast<cocos2d::BorderAlignment>(0));
}

} // namespace afpc