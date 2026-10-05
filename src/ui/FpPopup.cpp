#include "FpPopup.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <system_error>

#include <Geode/Geode.hpp>
#include <Geode/binding/ButtonSprite.hpp>
#include <Geode/binding/CCMenuItemSpriteExtra.hpp>
#include <Geode/cocos/label_nodes/CCLabelBMFont.h>
#include <Geode/cocos/menu_nodes/CCMenu.h>
#include <Geode/loader/Mod.hpp>

#include "../analysis/ClickLog.hpp"
#include "../game/GameSession.hpp"
#include "FramesPopup.hpp"

// cocos2d classes are namespaced and Geode's own types live in `geode`. This file
// deliberately does not pull in geode::prelude, so the handful of names used
// below are imported explicitly instead.
using cocos2d::CCLabelBMFont;
using cocos2d::CCMenu;
using cocos2d::CCSprite;
using geode::Anchor;

namespace afpc {

namespace {

// Process-global selection cursor. See the note in FpPopup.hpp: it has to outlive
// any individual popup so the choice survives close/reopen.
std::size_t g_index = 0;

// Extensions the ingest path can actually decode. A file that does not appear
// here would only ever be offered and then rejected, so it is never listed.
constexpr const char* kMacroExtensions[] = {".macro", ".slc", ".json", ".csv", ".txt",
                                            ".gdr", ".gdr2"};

// Geometry. The popup is tall because the preset grid is eighteen buttons; the
// layout is fixed constants rather than a layout engine because none of these
// rows reflow, and a fixed grid keeps every button in the same place every time
// the popup opens.
constexpr float kPopupWidth = 400.f;
constexpr float kPopupHeight = 400.f;

constexpr float kStatusY = 176.f;
constexpr float kSpeedFirstY = 140.f;
constexpr float kSpeedRowStep = 32.f;
constexpr float kSpeedPerRow = 5;
constexpr float kSpeedSpacing = 76.f;
constexpr float kOffsetY = 4.f;
constexpr float kMacroNavY = -50.f;
constexpr float kActionY = -110.f;

// The grid captions are bare numbers, so the buttons are scaled down to keep five
// of them on a row without the glyphs touching.
constexpr float kSpeedButtonScale = 0.62f;

// Opacity of an unselected preset. High enough to still be readable and clickable,
// low enough that the selected one is obvious without a colour dependency. Typed as
// unsigned char rather than GLubyte so this file does not need the GL headers.
constexpr unsigned char kSpeedIdleOpacity = 110;
constexpr unsigned char kSpeedActiveOpacity = 255;

[[nodiscard]] std::string lowered(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

// Lists the decodable macro files in <saveDir>/macros/, sorted so the Prev/Next
// order is stable across openings. Every error is swallowed with an error_code:
// a missing or unreadable directory is an ordinary state here, not an exception.
[[nodiscard]] std::vector<std::filesystem::path> scanMacros(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> out;

    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return out;

    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;

        const std::string ext = lowered(entry.path().extension().string());
        for (const char* candidate : kMacroExtensions) {
            if (ext == candidate) {
                out.push_back(entry.path());
                break;
            }
        }
    }

    std::sort(out.begin(), out.end());
    return out;
}

// Trims a rate to something that reads as a rate: "4" not "4.000000".
[[nodiscard]] std::string rateText(double rate) {
    if (rate <= 0.0) return "0";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", rate);
    return std::string(buf);
}

} // namespace

FpPopup* FpPopup::create() {
    auto* ret = new FpPopup();
    if (ret->initAnchored(kPopupWidth, kPopupHeight)) {
        ret->autorelease();
        return ret;
    }
    delete ret;
    return nullptr;
}

std::filesystem::path FpPopup::macrosDir() {
    auto* mod = geode::Mod::get();
    if (mod == nullptr) return {};
    return mod->getSaveDir() / "macros";
}

bool FpPopup::initAnchored(float width, float height) {
    if (!Popup::init(width, height, "GJ_square01.png")) return false;

    this->setID("novabot.autofpcount/popup");

    m_files = scanMacros(macrosDir());
    if (m_files.empty()) {
        g_index = 0;
    } else if (g_index >= m_files.size()) {
        // The file list shrank since the popup was last open (a macro was deleted).
        g_index = m_files.size() - 1;
    }
    m_index = g_index;

    m_statusLabel = CCLabelBMFont::create("", "goldFont.fnt");
    m_statusLabel->setScale(0.46f);
    m_mainLayer->addChildAtPosition(m_statusLabel, Anchor::Center, ccp(0.f, kStatusY));

    // Items are built at the call site rather than from a {caption, handler}
    // table: menu_selector only accepts a member function named directly where it
    // is used, so a stored pointer-to-member cannot be passed through it.
    auto* menu = CCMenu::create();
    menu->setID("novabot.autofpcount/menu");
    m_mainLayer->addChildAtPosition(menu, Anchor::Center);

    auto addItem = [this, menu](CCMenuItemSpriteExtra* item, float x, float y) {
        menu->addChild(item);
        item->setPosition(x, y);
    };

    // -- speed preset grid -------------------------------------------------
    // One handler serves all eighteen buttons; the pressed item's tag carries the
    // preset index.
    for (int i = 0; i < kSpeedPresetCount; ++i) {
        const int row = i / static_cast<int>(kSpeedPerRow);
        const int column = i % static_cast<int>(kSpeedPerRow);

        // Centre the final short row rather than leaving it hugging the left.
        const int inRow = std::min(static_cast<int>(kSpeedPerRow), kSpeedPresetCount - row * static_cast<int>(kSpeedPerRow));
        const float xOffset =
            (static_cast<float>(column) - (static_cast<float>(inRow) - 1.f) * 0.5f) * kSpeedSpacing;

        auto* sprite = ButtonSprite::create(rateText(kSpeedPresets[i]).c_str());
        sprite->setScale(kSpeedButtonScale);

        auto* item = CCMenuItemSpriteExtra::create(sprite, this, menu_selector(FpPopup::onSpeedPreset));
        item->setTag(i);
        m_speedSprites[static_cast<std::size_t>(i)] = sprite;

        addItem(item, xOffset, kSpeedFirstY - kSpeedRowStep * static_cast<float>(row));
    }

    // -- click offset ------------------------------------------------------
    // The value sits between the two arrows as a label rather than inside a text
    // field: it is a bounded integer nudged one tick at a time, and a spinner is
    // both quicker to hit and impossible to leave in an invalid state.
    addItem(CCMenuItemSpriteExtra::create(ButtonSprite::create("<"),
                                          this, menu_selector(FpPopup::onOffsetDown)),
            -124.f, kOffsetY);

    m_offsetLabel = CCLabelBMFont::create("", "chatFont.fnt");
    m_offsetLabel->setScale(0.55f);
    m_mainLayer->addChildAtPosition(m_offsetLabel, Anchor::Center, ccp(0.f, kOffsetY));

    addItem(CCMenuItemSpriteExtra::create(ButtonSprite::create(">"),
                                          this, menu_selector(FpPopup::onOffsetUp)),
            124.f, kOffsetY);

    // -- macro navigation and actions --------------------------------------
    addItem(CCMenuItemSpriteExtra::create(ButtonSprite::create("Previous"),
                                          this, menu_selector(FpPopup::onPrevious)),
            -110.f, kMacroNavY);
    addItem(CCMenuItemSpriteExtra::create(ButtonSprite::create("Next"),
                                          this, menu_selector(FpPopup::onNext)),
            110.f, kMacroNavY);

    addItem(CCMenuItemSpriteExtra::create(ButtonSprite::create("Reload"),
                                          this, menu_selector(FpPopup::onReload)),
            -140.f, kActionY);
    addItem(CCMenuItemSpriteExtra::create(ButtonSprite::create("Frames"),
                                          this, menu_selector(FpPopup::onFrames)),
            0.f, kActionY);
    addItem(CCMenuItemSpriteExtra::create(ButtonSprite::create("Export"),
                                          this, menu_selector(FpPopup::onExport)),
            140.f, kActionY);

    refreshStatus();
    return true;
}

void FpPopup::selectIndex(std::size_t index) {
    if (m_files.empty()) {
        m_index = 0;
    } else {
        m_index = index % m_files.size();
    }
    g_index = m_index;
}

void FpPopup::loadSelected() {
    if (m_files.empty()) return;
    GameSession::get().importMacroFile(m_files[m_index]);
    refreshStatus();
}

void FpPopup::refreshStatus() {
    if (m_statusLabel == nullptr) return;

    auto& session = GameSession::get();

    const std::string name = m_files.empty()
                                 ? std::string("(no macros found)")
                                 : m_files[m_index].filename().string();

    // The achieved rate is shown next to the requested one because they are not
    // the same number: the per-frame tick ceiling caps what is reachable, so
    // reporting only the requested figure would overstate what the game is doing.
    const double requested = session.requestedPlaybackRate();
    const double realised = session.realisedPlaybackRate();

    std::string speedLine = "Speed: " + rateText(requested) + "x";
    if (requested > 1.0) {
        // Before playback has driven a single tick the measurement is 0, not a
        // real rate. Reporting "0.0x act" would read as playback having stalled.
        speedLine += " req / ";
        speedLine += realised > 0.0 ? (rateText(realised) + "x act") : std::string("idle");
    }

    const std::string& diagnostics = session.macroDiagnostics();

    std::string text = "Macro: " + name + "\n";
    text += speedLine + "\n";
    text += diagnostics.empty() ? std::string("Gate: not loaded") : ("Gate: " + diagnostics);
    text += "\nClicks: " + std::to_string(session.clickLog().size()) +
            "   overrides: " + std::to_string(session.clickLog().overriddenCount());

    m_statusLabel->setString(text.c_str());

    // -- offset readout ----------------------------------------------------
    const int offset = session.clickOffset();
    const std::string offsetText = "offset " + (offset == 0 ? std::string("0")
                                                           : (offset > 0 ? "+" : "")) +
                                   std::to_string(offset) + "t";
    m_offsetLabel->setString(offsetText.c_str());

    // -- preset grid tint --------------------------------------------------
    const int active = session.activeSpeedPreset();
    for (int i = 0; i < kSpeedPresetCount; ++i) {
        auto* sprite = m_speedSprites[static_cast<std::size_t>(i)];
        if (sprite == nullptr) continue;
        sprite->setOpacity(i == active ? kSpeedActiveOpacity : kSpeedIdleOpacity);
    }
}

void FpPopup::onPrevious(CCObject*) {
    if (m_files.empty()) return;
    selectIndex(m_index == 0 ? m_files.size() - 1 : m_index - 1);
    loadSelected();
}

void FpPopup::onNext(CCObject*) {
    if (m_files.empty()) return;
    selectIndex(m_index + 1);
    loadSelected();
}

void FpPopup::onReload(CCObject*) {
    // Re-scan first: a macro may have been dropped in since the popup opened.
    m_files = scanMacros(macrosDir());
    if (!m_files.empty() && m_index >= m_files.size()) m_index = m_files.size() - 1;
    loadSelected();
}

void FpPopup::onSpeedPreset(CCObject* sender) {
    auto* item = static_cast<CCMenuItemSpriteExtra*>(sender);
    if (item == nullptr) return;

    GameSession::get().setSpeedPreset(item->getTag());
    refreshStatus();
}

void FpPopup::onOffsetDown(CCObject*) {
    auto& session = GameSession::get();
    session.setClickOffset(session.clickOffset() - 1);
    refreshStatus();
}

void FpPopup::onOffsetUp(CCObject*) {
    auto& session = GameSession::get();
    session.setClickOffset(session.clickOffset() + 1);
    refreshStatus();
}

void FpPopup::onFrames(CCObject*) {
    // The list is opened with the FP popup still behind it rather than replacing
    // it, so returning to the controls is a single close rather than a reopen.
    if (auto* frames = FramesPopup::create()) frames->show();
}

void FpPopup::onExport(CCObject*) {
    std::string error;
    const bool ok = GameSession::get().exportRecording(
        Recorder::defaultExportPath(), MacroFormat::Unknown, &error);

    if (m_statusLabel == nullptr) return;
    m_statusLabel->setString(ok ? "Exported capture.macro" : ("Export failed: " + error).c_str());
}

} // namespace afpc
