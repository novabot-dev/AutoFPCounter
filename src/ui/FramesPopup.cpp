#include "FramesPopup.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>

#include <Geode/Geode.hpp>
#include <Geode/binding/ButtonSprite.hpp>
#include <Geode/binding/CCMenuItemSpriteExtra.hpp>
#include <Geode/cocos/menu_nodes/CCMenu.h>

#include "../analysis/ClickLog.hpp"
#include "../game/GameSession.hpp"

using cocos2d::CCLabelBMFont;
using cocos2d::CCMenu;
using geode::Anchor;
using geode::TextInput;

namespace afpc {

namespace {

// ---------------------------------------------------------------------------
// Geometry
//
// Bounded by the screen the same way the FP popup is: GD's common win size is
// 569x320, so *height* is the binding constraint and anything past ~300 runs off
// the top and bottom. Width is not the constraint. This keeps seven rows rather
// than the original eight - a row too small to read is worse than one row fewer,
// since the Previous/Next buttons page through the rest.
//
// kPopupHeight is 296, matching the FP popup, so the two are the same size and
// do not jump when the user moves between them. Half-height is 148.
//
// Vertical budget, top to bottom, in half-height units (148 of 148):
//   +128  header
//   +106  first row, seven rows at a 20 step -> last row at -14
//    -46  text input
//    -84  Previous / Next / Edit / Apply
//   -118  Reset override
// Bottom edge of the last band is -118-17 = -135, inside the -148 floor.
// ---------------------------------------------------------------------------
constexpr float kPopupWidth = 330.f;
constexpr float kPopupHeight = 296.f;

constexpr float kHeaderY = 128.f;

// Seven rows at a 20 step, so the last row sits at 106 - 6*20 = -14, just above
// the text input at -46. This is the tightest band in the popup: the rows carry
// the most information in the least space, and 0.4 is what keeps chatFont legible
// at a 20 step. The count itself lives in the header, which sizes the row pool.
constexpr float kFirstRowY = 106.f;
constexpr float kRowStep = 20.f;
constexpr float kInputY = -46.f;
constexpr float kButtonY = -84.f;
constexpr float kResetY = -118.f;

// Four buttons across a 330 popup leaves roughly 78 per button, which is enough
// for "Previous" at the scale below without the captions touching.
constexpr float kFourUpX = 118.f;

// Label scale. The rows carry the most information in the smallest space, so
// they are set a little smaller than the header but stay above the size at which
// chatFont glyphs stop being readable.
constexpr float kHeaderScale = 0.44f;
constexpr float kRowScale = 0.4f;
constexpr float kButtonScale = 0.74f;

} // namespace

FramesPopup* FramesPopup::create() {
    auto* ret = new FramesPopup();
    if (ret->initAnchored(kPopupWidth, kPopupHeight)) {
        ret->autorelease();
        return ret;
    }
    delete ret;
    return nullptr;
}

int FramesPopup::logSize() const {
    return GameSession::get().clickLog().size();
}

bool FramesPopup::initAnchored(float width, float height) {
    if (!Popup::init(width, height, "GJ_square01.png")) return false;

    this->setID("novabot.autofpcount/frames");

    m_header = CCLabelBMFont::create("", "goldFont.fnt");
    m_header->setScale(kHeaderScale);
    m_mainLayer->addChildAtPosition(m_header, Anchor::Center, ccp(0.f, kHeaderY));

    for (int i = 0; i < kVisibleRows; ++i) {
        m_rows[static_cast<std::size_t>(i)] = CCLabelBMFont::create("", "chatFont.fnt");
        m_rows[static_cast<std::size_t>(i)]->setScale(kRowScale);
        m_mainLayer->addChildAtPosition(
            m_rows[static_cast<std::size_t>(i)], Anchor::Center,
            ccp(0.f, kFirstRowY - kRowStep * static_cast<float>(i)));
    }

    // Exactly one TextInput for the whole list. Its change callback is the only
    // way text is read, which avoids a gd::string round trip in this file.
    m_input = TextInput::create(190.f, "frames");
    m_input->setID("novabot.autofpcount/frame-override");
    m_input->setCallback([this](std::string const& text) { m_pendingText = text; });
    m_input->setString("", false);
    m_mainLayer->addChildAtPosition(m_input, Anchor::Center, ccp(0.f, kInputY));

    // Items are built at the call site rather than from a {caption, handler}
    // table: menu_selector only accepts a member function named directly where
    // it is used, so a stored pointer-to-member cannot be passed through it.
    auto* menu = CCMenu::create();
    menu->setID("novabot.autofpcount/frames-menu");
    m_mainLayer->addChildAtPosition(menu, Anchor::Center);

    auto addItem = [menu](CCMenuItemSpriteExtra* item, float x, float y) {
        menu->addChild(item);
        item->setPosition(x, y);
    };

    // Returns a scaled sprite for a word-caption button. It deliberately does not
    // take the handler: menu_selector only accepts a member function named
    // directly at the call site, so a stored pointer-to-member cannot be passed
    // through it. Callers build the item themselves and name the handler.
    auto captionSprite = [](const char* caption) {
        auto* sprite = ButtonSprite::create(caption);
        sprite->setScale(kButtonScale);
        return sprite;
    };

    // Four-up, laid out from the outer column so the pitch is derived rather
    // than hard-coded: kFourUpX is the centre of the outermost button and the
    // inner pair sits two thirds of the way in.
    const float kInnerX = kFourUpX / 3.f;

    addItem(CCMenuItemSpriteExtra::create(captionSprite("Previous"),
                                         this, menu_selector(FramesPopup::onPrevious)),
            -kFourUpX, kButtonY);
    addItem(CCMenuItemSpriteExtra::create(captionSprite("Next"),
                                         this, menu_selector(FramesPopup::onNext)),
            -kInnerX, kButtonY);
    addItem(CCMenuItemSpriteExtra::create(captionSprite("Edit"),
                                         this, menu_selector(FramesPopup::onEdit)),
            kInnerX, kButtonY);
    addItem(CCMenuItemSpriteExtra::create(captionSprite("Apply"),
                                         this, menu_selector(FramesPopup::onApply)),
            kFourUpX, kButtonY);
    addItem(CCMenuItemSpriteExtra::create(captionSprite("Reset override"),
                                         this, menu_selector(FramesPopup::onReset)),
            0.f, kResetY);

    refresh();
    return true;
}

void FramesPopup::ensureCursorVisible() {
    const int size = logSize();
    if (size <= 0) {
        m_first = 0;
        m_cursor = 0;
        return;
    }

    m_cursor = std::clamp(m_cursor, 0, size - 1);

    if (m_cursor < m_first) {
        m_first = m_cursor;
    } else if (m_cursor >= m_first + kVisibleRows) {
        m_first = m_cursor - kVisibleRows + 1;
    }

    // Do not scroll past the end: with fewer clicks than rows the window sits at
    // the top and the empty rows below stay empty.
    m_first = std::clamp(m_first, 0, std::max(0, size - kVisibleRows));
}

void FramesPopup::refresh() {
    const ClickLog& log = GameSession::get().clickLog();
    const int size = log.size();

    ensureCursorVisible();

    if (size == 0) {
        m_header->setString("No clicks recorded this attempt yet");
        for (auto* row : m_rows) row->setString("");
        return;
    }

    const ClickRecord* selected = log.atOrdinal(m_cursor);
    const unsigned overridden = log.overriddenCount();

    m_header->setString(
        ("Click " + std::to_string(m_cursor + 1) + " of " + std::to_string(size) +
         "   overrides: " + std::to_string(overridden))
            .c_str());

    for (int i = 0; i < kVisibleRows; ++i) {
        auto* label = m_rows[static_cast<std::size_t>(i)];
        const int ordinal = m_first + i;

        if (ordinal >= size) {
            label->setString("");
            continue;
        }

        const ClickRecord* record = log.atOrdinal(ordinal);
        if (record == nullptr) {
            label->setString("");
            continue;
        }

        // The marker distinguishes the selected row at a glance, and the trailing
        // note keeps the original measurement visible next to the corrected one so
        // an override can never hide the evidence it replaces.
        std::string text;
        text.reserve(64);
        text += ordinal == m_cursor ? ">" : " ";
        text += "#" + std::to_string(record->sequence) +
                "  FRAME: " + std::to_string(record->effective.rawFrames) +
                "  " + record->effective.label;
        if (record->hasOverride) {
            text += "  (measured " + std::to_string(record->analyzed.rawFrames) + ")";
        }
        label->setString(text.c_str());
    }

    if (selected != nullptr && !m_editing) {
        m_input->setString(std::to_string(selected->effective.rawFrames).c_str(), false);
        m_pendingText = std::to_string(selected->effective.rawFrames);
    }
}

void FramesPopup::onPrevious(CCObject*) {
    if (m_cursor > 0) --m_cursor;
    m_editing = false;
    m_input->defocus();
    refresh();
}

void FramesPopup::onNext(CCObject*) {
    if (m_cursor + 1 < logSize()) ++m_cursor;
    m_editing = false;
    m_input->defocus();
    refresh();
}

void FramesPopup::onEdit(CCObject*) {
    if (logSize() == 0) return;

    // Seeded from the selected click so the user edits the existing value rather
    // than retyping it. If the click already carries an override this re-seeds
    // from the override, which is what a second correction should start from.
    const ClickRecord* record = GameSession::get().clickLog().atOrdinal(m_cursor);
    if (record == nullptr) return;

    const std::string current = std::to_string(record->effective.rawFrames);
    m_input->setString(current.c_str(), false);
    m_pendingText = current;

    m_editing = true;
    m_input->focus();
}

void FramesPopup::onApply(CCObject*) {
    if (logSize() == 0) return;

    const ClickRecord* record = GameSession::get().clickLog().atOrdinal(m_cursor);
    if (record == nullptr) return;

    // Parsed leniently: atoi yields 0 for empty or non-numeric input, and
    // ClickLog::setOverride clamps that to the tightest window rather than
    // dividing by zero. The user gets a visible 1 in the list rather than a
    // silently ignored keypress.
    const int frames = std::atoi(m_pendingText.c_str());

    GameSession::get().setClickWindowOverride(record->sequence, frames);

    m_editing = false;
    m_input->defocus();
    refresh();
}

void FramesPopup::onReset(CCObject*) {
    if (logSize() == 0) return;

    const ClickRecord* record = GameSession::get().clickLog().atOrdinal(m_cursor);
    if (record == nullptr) return;

    GameSession::get().clearClickWindowOverride(record->sequence);

    m_editing = false;
    m_input->defocus();
    refresh();
}

} // namespace afpc
