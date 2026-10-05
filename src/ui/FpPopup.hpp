#pragma once

#include <Geode/cocos/label_nodes/CCLabelBMFont.h>
#include <Geode/ui/Popup.hpp>
#include <Geode/utils/async.hpp>
#include <Geode/utils/file.hpp>

#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "../core/Constants.hpp"

// CCNode-derived types used by this header are Geode cocos bindings and live in
// `cocos2d`. Forward declaring them here keeps this header cheap to include.
namespace cocos2d {
class CCLabelBMFont;
class CCSprite;
}

namespace afpc {

// ---------------------------------------------------------------------------
// FpPopup
//
// The control panel reachable from the "FP" button on the pause menu.
//
// Everything here is plain file-system and engine state - no networking, no
// threads, no allocation per frame. The popup is built once in initAnchored() and
// only its labels and the preset grid's opacity are rewritten when the user
// changes something, so opening and closing it costs nothing measurable.
//
// SELECTION STATE
// ---------------
// The chosen macro is process-global rather than per-popup. A user who picks
// "level.slc", closes the popup and reopens it expects to still be looking at
// "level.slc", so that cursor lives in the translation unit instead of in the
// (short-lived) popup object. The playback speed, by contrast, lives in
// GameSession - it has to outlive the popup and survive a practice-mode retry.
// ---------------------------------------------------------------------------
class FpPopup : public geode::Popup {
public:
    // Geode 5's Popup is a plain base class rather than a CRTP template, so the
    // factory is spelled out instead of being inherited.
    static FpPopup* create();

protected:
    // Popup::init is protected and not virtual, so it is wrapped here rather than
    // overridden, then the static factory calls this.
    bool initAnchored(float width, float height);

    void onPrevious(CCObject* sender);
    void onNext(CCObject* sender);
    void onReload(CCObject* sender);
    void onExport(CCObject* sender);
    void onFrames(CCObject* sender);

    // Opens the system file picker and ingests the chosen file. The picked file
    // is copied into the macros folder first, so it joins the Prev/Next list and
    // survives a restart rather than living at whatever path it was browsed from.
    void onImport(CCObject* sender);

    // Opens the macros folder in the system file browser, creating it if needed.
    void onOpenFolder(CCObject* sender);

    // Runs on the main thread once the picker closes. `pickedPath` is empty when
    // the dialog was dismissed. Not part of the popup's UI surface.
    void applyPickedFile(bool failed, std::string pickedPath);

    void onOffsetDown(CCObject* sender);
    void onOffsetUp(CCObject* sender);

    // One handler for the whole preset grid. The pressed item's tag carries the
    // preset index, so eighteen buttons need one method rather than eighteen.
    void onSpeedPreset(CCObject* sender);

    // Rewrites the status, offset and preset labels from the current GameSession
    // state, and re-tints the preset grid.
    void refreshStatus();

    void selectIndex(std::size_t index);
    void loadSelected();

    [[nodiscard]] std::filesystem::path macrosDir();

    cocos2d::CCLabelBMFont* m_statusLabel = nullptr;
    cocos2d::CCLabelBMFont* m_offsetLabel = nullptr;

    // Held so the grid can be re-tinted. The selected preset is drawn at full
    // opacity and the rest dimmed, which is cheaper and less error-prone than
    // rebuilding eighteen sprites on every change.
    std::array<cocos2d::CCSprite*, kSpeedPresetCount> m_speedSprites{};

    std::vector<std::filesystem::path> m_files;
    std::size_t m_index = 0;

    // Holds the in-flight file picker. It is a member rather than a local because
    // TaskHolder cancels the task when destroyed: if the user closes the popup
    // while the system dialog is open, the callback must not fire into a
    // destroyed popup.
    geode::async::TaskHolder<geode::utils::file::PickResult> m_picker;

    // Set when the user picked something the importer refused. Held separately
    // from GameSession's diagnostics so the failure survives refreshStatus() and
    // is not immediately overwritten by the next status line.
    std::string m_importError;

    // Rebuilds m_files from disk and reselects the entry matching `preferred`,
    // falling back to the current index. Used after an import so the new file
    // appears in the list and is the selected one.
    void rescanMacros(const std::filesystem::path& preferred);
};

} // namespace afpc
