#include "FpPopup.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <string>
#include <system_error>
#include <utility>

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
// ---------------------------------------------------------------------------
// Geometry
//
// Sizing is driven by the screen, not by taste. Geometry Dash's win size is
// 569x320 in the common 16:9 case, so *height* is the binding constraint: any
// popup taller than ~300 runs off the top and bottom, which is what the previous
// 400x400 layout did. Width is not the problem - 344 sits comfortably inside 569 -
// so the budget below is spent purely on getting the height under the screen.
//
// kPopupHeight is 296, leaving 24px of screen for the popup's drop shadow and
// the 10px of slack needed because Geode popups are centred, not top-aligned.
// Half-height is therefore 148, and every band below is placed against that.
//
// Vertical budget, top to bottom, in half-height units (148 of 148):
//   +124  status block, four lines of 0.42-scaled goldFont
//    +92  preset grid row 1
//    +68  preset grid row 2
//    +44  preset grid row 3
//    +12  click offset spinner
//    -22  macro Previous / Next
//    -58  Reload / Frames / Export
//    -94  Import / Folder
// Lowest band bottom edge is -94-17 = -111, clear of the -148 floor by 37.
// ---------------------------------------------------------------------------
constexpr float kPopupWidth = 344.f;
constexpr float kPopupHeight = 296.f;

constexpr float kStatusY = 124.f;

// Six per row instead of five: eighteen presets then fit in three rows rather
// than four. This is the single biggest saving in the vertical budget - the grid
// is the tallest thing in the popup and three rows beat four by 24px.
constexpr int kSpeedPerRow = 6;
constexpr float kSpeedFirstY = 92.f;
constexpr float kSpeedRowStep = 24.f;

// 52 of column pitch at 0.58 scale keeps a six-wide row inside a 344 popup with
// margin on both sides. Widest grid row is +/-130, inside the +/-172 half-width.
constexpr float kSpeedSpacing = 52.f;

constexpr float kOffsetY = 12.f;
constexpr float kMacroNavY = -22.f;
constexpr float kActionY = -58.f;
constexpr float kImportY = -94.f;

// Column centres for the three-item rows. The widest captions are "Previous",
// "Reload" and "Import", so these are tuned against that rather than the
// single-character arrows in the offset row.
constexpr float kThreeUpX = 104.f;
constexpr float kTwoUpX = 68.f;
constexpr float kTwoUpWideX = 78.f;

// The offset arrows flank a readout, so they sit further out than the two-up
// rows: there is a label between them.
constexpr float kArrowX = 118.f;

// The grid captions are bare numbers, so the buttons are scaled down to keep six
// of them on a row without the glyphs touching. At this scale a row button is
// about 30px tall, which is what the 24px row step is measured against.
constexpr float kSpeedButtonScale = 0.58f;

// Word-caption buttons need less shrink than the grid does, but still enough that
// "Previous" and "Reload" clear each other at the spacing above. 0.82 is the
// largest value that keeps a three-up row inside kPopupWidth.
constexpr float kActionButtonScale = 0.82f;

// The status block is four lines of goldFont. goldFont is a wide face - roughly
// 12px per glyph at scale 1 - so the scale here is set by the longest line that
// can appear, not by taste. "Gate: Silicate / 1200 inputs / 240.000000 FPS" is
// about 45 glyphs, which at 0.42 needs ~227px of the 344 available.
constexpr float kStatusScale = 0.42f;

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

// Folds a diagnostics string onto one line and caps it.
//
// Two separate problems are solved here. Diagnostics arrive from three places
// with different shapes: a rejection sets them to "<filename>\n\n<message>", a
// successful ingest sets them to "<format> / <n> inputs / <fps> FPS", and the
// gate-disabled suffix can append a 60-character warning. Embedded newlines
// would push the status label's line count past the four the vertical budget
// allows, and an over-long line would run past the popup edge - neither is
// recoverable once the text is already in the label.
//
// The full text is never lost: GameSession logs it and raises it in an alert
// popup on every rejection, so this is only about what fits on one line here.
[[nodiscard]] std::string singleLine(std::string text, std::size_t maxChars) {
    for (char& c : text) {
        if (c == '\n' || c == '\r') c = ' ';
    }

    // Collapse the runs of spaces left behind, so "name\n\nreason" reads as
    // "name reason" rather than "name  reason".
    std::string out;
    out.reserve(text.size());
    bool lastWasSpace = false;
    for (const char c : text) {
        const bool isSpace = c == ' ';
        if (isSpace && lastWasSpace) continue;
        out += c;
        lastWasSpace = isSpace;
    }

    if (out.size() > maxChars) {
        // Cut on a UTF-8 boundary: goldFont cannot render a dangling lead byte,
        // and a partial glyph would draw as a box or drop the whole line.
        std::size_t cut = maxChars;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;

        out.resize(cut);
        out += "...";
    }
    return out;
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
    m_statusLabel->setScale(kStatusScale);
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

    // Returns a scaled sprite for a word-caption button. This deliberately does
    // *not* take the handler: menu_selector only accepts a member function named
    // directly at the call site, so a stored pointer-to-member cannot be passed
    // through it. Callers build the item themselves and pass the handler by name.
    auto captionSprite = [](const char* caption) {
        auto* sprite = ButtonSprite::create(caption);
        sprite->setScale(kActionButtonScale);
        return sprite;
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
    //
    // The arrows keep the grid's button scale. They are single characters, so at
    // 0.58 they are still a comfortable tap target, and using a second scale here
    // would make the two arrow buttons the only oversized pair in the popup.
    auto* downSprite = ButtonSprite::create("<");
    downSprite->setScale(kSpeedButtonScale);
    addItem(CCMenuItemSpriteExtra::create(downSprite, this, menu_selector(FpPopup::onOffsetDown)),
            -kArrowX, kOffsetY);

    m_offsetLabel = CCLabelBMFont::create("", "chatFont.fnt");
    m_offsetLabel->setScale(0.5f);
    m_mainLayer->addChildAtPosition(m_offsetLabel, Anchor::Center, ccp(0.f, kOffsetY));

    auto* upSprite = ButtonSprite::create(">");
    upSprite->setScale(kSpeedButtonScale);
    addItem(CCMenuItemSpriteExtra::create(upSprite, this, menu_selector(FpPopup::onOffsetUp)),
            kArrowX, kOffsetY);

    // -- macro navigation and actions --------------------------------------
    addItem(CCMenuItemSpriteExtra::create(captionSprite("Previous"),
                                          this, menu_selector(FpPopup::onPrevious)),
            -kTwoUpWideX, kMacroNavY);
    addItem(CCMenuItemSpriteExtra::create(captionSprite("Next"),
                                          this, menu_selector(FpPopup::onNext)),
            kTwoUpWideX, kMacroNavY);

    addItem(CCMenuItemSpriteExtra::create(captionSprite("Reload"),
                                          this, menu_selector(FpPopup::onReload)),
            -kThreeUpX, kActionY);
    addItem(CCMenuItemSpriteExtra::create(captionSprite("Frames"),
                                          this, menu_selector(FpPopup::onFrames)),
            0.f, kActionY);
    addItem(CCMenuItemSpriteExtra::create(captionSprite("Export"),
                                          this, menu_selector(FpPopup::onExport)),
            kThreeUpX, kActionY);

    // -- import -----------------------------------------------------------
    // Its own row below the actions. Import is the one button a first-time user
    // needs, so it is placed where the eye lands last-but-one rather than being
    // folded into the Prev/Next pair, which only cycle files already on disk.
    addItem(CCMenuItemSpriteExtra::create(captionSprite("Import"),
                                          this, menu_selector(FpPopup::onImport)),
            -kTwoUpX, kImportY);
    addItem(CCMenuItemSpriteExtra::create(captionSprite("Folder"),
                                          this, menu_selector(FpPopup::onOpenFolder)),
            kTwoUpX, kImportY);

    refreshStatus();
    return true;
}

void FpPopup::onOpenFolder(CCObject*) {
    // Opens the macros directory in Explorer. Dropping a file in there by hand is
    // the other way to get a macro loaded, so this is the shortcut for it.
    //
    // openFolder shells out, which is unavailable on a console build or under
    // some sandboxing setups, and it reports that by returning false rather than
    // throwing. Showing the path regardless means the button is still useful as
    // "where do I put this?", which is the more common reason to press it.
    const auto dir = macrosDir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    if (!geode::utils::file::openFolder(dir)) {
        m_importError = singleLine("Folder: " + dir.string(), 64);
        refreshStatus();
    }
}

void FpPopup::rescanMacros(const std::filesystem::path& preferred) {
    m_files = scanMacros(macrosDir());

    if (m_files.empty()) {
        m_index = 0;
        g_index = 0;
        return;
    }

    // Select the file just imported rather than leaving the cursor where it was,
    // so the status line names the file the user just chose.
    m_index = 0;
    if (!preferred.empty()) {
        const auto it = std::find(m_files.begin(), m_files.end(), preferred);
        if (it != m_files.end()) m_index = static_cast<std::size_t>(it - m_files.begin());
    } else if (g_index >= m_files.size()) {
        m_index = m_files.size() - 1;
    } else {
        m_index = g_index;
    }
    g_index = m_index;
}

void FpPopup::onImport(CCObject*) {
    if (m_picker.isPending()) return; // a dialog is already up

    m_importError.clear();

    geode::utils::file::FilePickOptions options;
    options.defaultPath = macrosDir();

    // One filter per accepted extension, all under a single heading. The system
    // dialog filters what it shows, so listing them means a .gdr2 is visible
    // rather than the file appearing to not exist.
    geode::utils::file::FilePickOptions::Filter filter;
    filter.description = "Macro replays";
    for (const char* ext : kMacroExtensions) {
        filter.files.insert(ext);
    }
    options.filters.push_back(std::move(filter));

    // `pick` is ARC-based and returns immediately, so it cannot be waited on
    // with co_await here. TaskHolder spawns it and delivers the result back on
    // the main thread, which is also the only thread allowed to touch the UI.
    m_picker.spawn(
        "autofpcount-import",
        geode::utils::file::pick(geode::utils::file::PickMode::OpenFile, std::move(options)),
        [this](geode::utils::file::PickResult result) {
            // An error here means the dialog itself could not be shown, which is
            // rare but worth a line in the status area rather than silence.
            if (!result) {
                applyPickedFile(true, "file dialog: " + result.unwrapErr());
                return;
            }
            // A nullopt path means the user dismissed the dialog. That is not an
            // error, so it reports nothing at all.
            //
            // The result is unwrapped once into a local rather than twice: unwrap()
            // hands back a reference, so calling it per-use is both needlessly
            // repeated and easy to get wrong.
            const auto& picked = result.unwrap();
            if (!picked.has_value()) return;

            // path has no implicit conversion to string - the conversion is
            // explicit via .string(), because the encoding is a real decision on
            // Windows rather than a formality.
            applyPickedFile(false, picked.value().string());
        }
    );
}

void FpPopup::applyPickedFile(bool failed, std::string pickedPath) {
    if (failed) {
        m_importError = "Import failed: " + pickedPath;
        refreshStatus();
        return;
    }
    if (pickedPath.empty()) return; // user dismissed the dialog

    std::error_code ec;
    const std::filesystem::path source = pickedPath;

    const std::string ext = lowered(source.extension().string());
    bool accepted = false;
    for (const char* candidate : kMacroExtensions) {
        if (ext == candidate) {
            accepted = true;
            break;
        }
    }
    if (!accepted) {
        m_importError = "Unsupported file type '" + ext + "'";
        refreshStatus();
        return;
    }

    // Copy into the macros folder so the file joins the Prev/Next list, is
    // re-ingested automatically at the start of each attempt, and survives a
    // restart. Naming collisions get a numeric suffix rather than silently
    // overwriting an existing macro.
    const auto dir = macrosDir();
    std::filesystem::create_directories(dir, ec);

    std::filesystem::path destination = dir / source.filename().string();
    if (std::filesystem::exists(destination, ec)) {
        const auto stem = source.stem().string();
        for (int n = 1; n < 1000; ++n) {
            const std::string candidate = stem + "-" + std::to_string(n) + ext;
            const auto path = dir / candidate;
            if (!std::filesystem::exists(path, ec)) {
                destination = path;
                break;
            }
        }
    }

    std::filesystem::copy_file(source, destination,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        m_importError = "Cannot copy into the macros folder: " + ec.message();
        refreshStatus();
        return;
    }

    rescanMacros(destination);

    if (GameSession::get().importMacroFile(destination)) {
        m_importError.clear();
    } else {
        m_importError = "Rejected: " + GameSession::get().macroDiagnostics();
    }
    refreshStatus();
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

    // Glyph budget per line. goldFont is about 11.5px per glyph at scale 1, so at
    // kStatusScale of 0.42 that is ~4.8px each; 64 glyphs is ~307px of the 344
    // popup, leaving the frame's own inset on both sides.
    constexpr std::size_t kStatusChars = 64;

    std::string text = "Macro: " + singleLine(name, kStatusChars - 7) + "\n";
    text += speedLine + "\n";
    // An import failure outranks the gate line: it is the reason the user just
    // pressed a button, and burying it under a stale gate message is how a
    // refused file looks like a silent no-op.
    //
    // The gate line is where text is most likely to be long, because it carries
    // the format name, the input count, the declared rate and any parser
    // diagnostic all on one line. Left as four separate lines the label would
    // need a tiny scale to fit a 344-wide popup, so instead the rate is printed
    // the way rateText does it - "240", not "240.000000" - and the longest
    // possible line lands at roughly 45 glyphs.
    if (!m_importError.empty()) {
        text += singleLine(m_importError, kStatusChars);
    } else {
        text += diagnostics.empty() ? std::string("Gate: not loaded")
                                    : singleLine("Gate: " + diagnostics, kStatusChars);
    }
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
