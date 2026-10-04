#pragma once

#include <Geode/ui/Popup.hpp>

#include <cstddef>
#include <filesystem>
#include <vector>

// CCNode-derived types used by this header are Geode cocos bindings and live in
// `cocos2d`. Forward declaring them here keeps this header cheap to include.
namespace cocos2d {
class CCLabelBMFont;
}

namespace afpc {

// ---------------------------------------------------------------------------
// FpPopup
//
// The control panel reachable from the "FP" button on the pause menu.
//
// Everything here is plain file-system and engine state - no networking, no
// threads, no allocation per frame. The popup is built once in initAnchored() and
// only its status label is rewritten when the user changes something, so opening
// and closing it costs nothing measurable.
//
// SELECTION STATE
// ---------------
// The chosen macro is process-global rather than per-popup. A user who picks
// "level.slc", closes the popup and reopens it expects to still be looking at
// "level.slc", so the cursor lives in this translation unit instead of in the
// (short-lived) popup object. It is a plain non-atomic global because it is only
// ever touched from the game's main thread.
// ---------------------------------------------------------------------------
class FpPopup : public geode::Popup {
public:
    // Geode 5's Popup is a plain base class rather than a CRTP template, so the
    // factory is spelled out here instead of being inherited.
    static FpPopup* create();

protected:
    // Popup::init is protected and not virtual, so it is wrapped here rather than
    // overridden, then the static factory calls this.
    bool initAnchored(float width, float height);

    void onReload(CCObject* sender);
    void onPrevious(CCObject* sender);
    void onNext(CCObject* sender);
    void onAccelerated(CCObject* sender);
    void onExport(CCObject* sender);

    // Rewrites the status label from the current GameSession state.
    void refreshStatus();

    void selectIndex(std::size_t index);
    void loadSelected();

    [[nodiscard]] std::filesystem::path macrosDir();

    cocos2d::CCLabelBMFont* m_statusLabel = nullptr;
    std::vector<std::filesystem::path> m_files;
    std::size_t m_index = 0;
};

} // namespace afpc