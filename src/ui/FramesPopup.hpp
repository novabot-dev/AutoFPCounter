#pragma once

#include <Geode/cocos/label_nodes/CCLabelBMFont.h>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>

#include <array>
#include <cstddef>
#include <string>

namespace afpc {

// ---------------------------------------------------------------------------
// FramesPopup
//
// The per-click list behind the popup's "Frames" button. One row per classified
// click of the current attempt, showing the window the InteractionTracker
// measured, with a text field that lets the user correct it.
//
// WHY ONE SHARED TEXT FIELD RATHER THAN ONE PER ROW
// --------------------------------------------------
// An attempt can hold up to ClickLog::kCapacity (1024) clicks. A list of 1024
// live TextInput nodes is not a viable thing to build: each one is a real
// cocos2d node with its own touch delegate, so the list would allocate on every
// scroll and the frame time would be spent on the list rather than on physics.
// Instead the rows are a fixed recycled pool of plain labels and there is exactly
// one TextInput, bound to whichever row is selected. The user sees the same thing
// they asked for - a box next to the row - at a cost that does not scale with
// attempt length.
//
// The input's text is read through its change callback rather than getString(),
// which keeps a Geode gd::string out of this file entirely.
// ---------------------------------------------------------------------------
class FramesPopup : public geode::Popup {
public:
    // Geode 5's Popup is a plain base class rather than a CRTP template, so the
    // factory is spelled out rather than inherited.
    static FramesPopup* create();

protected:
    // Popup::init is protected and not virtual, so it is wrapped rather than
    // overridden, and the static factory calls this.
    bool initAnchored(float width, float height);

    void onPrevious(CCObject* sender);
    void onNext(CCObject* sender);
    void onEdit(CCObject* sender);
    void onApply(CCObject* sender);
    void onReset(CCObject* sender);

    // Repaints the header and every visible row from the current log and cursor.
    void refresh();

    // Scrolls m_first so the selected ordinal is inside the window, then clamps
    // both ends.
    void ensureCursorVisible();

    [[nodiscard]] int logSize() const;

    // Eight rows is enough to judge a pattern at a glance while keeping every
    // label on screen inside the popup at a legible scale.
    static constexpr int kVisibleRows = 8;

    cocos2d::CCLabelBMFont* m_header = nullptr;
    std::array<cocos2d::CCLabelBMFont*, kVisibleRows> m_rows{};

    geode::TextInput* m_input = nullptr;

    // Latest text seen from the input's change callback. Seeded on Edit so Apply
    // works even if the user focuses the field and types nothing.
    std::string m_pendingText;

    // Ordinals index the log newest-first: 0 is the most recent click. m_first is
    // the topmost visible row, m_cursor the selected one.
    int m_first = 0;
    int m_cursor = 0;

    bool m_editing = false;
};

} // namespace afpc
