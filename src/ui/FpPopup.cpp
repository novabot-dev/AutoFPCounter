#include "FpPopup.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <system_error>

#include <Geode/Geode.hpp>
#include <Geode/binding/ButtonSprite.hpp>
#include <Geode/binding/CCLabelBMFont.hpp>
#include <Geode/binding/CCMenu.hpp>
#include <Geode/binding/CCMenuItemSpriteExtra.hpp>
#include <Geode/loader/Mod.hpp>

#include "../game/GameSession.hpp"

namespace afpc {

namespace {

// Process-global selection cursor. See the note in FpPopup.hpp: it has to outlive
// any individual popup so the choice survives close/reopen.
std::size_t g_index = 0;

// Extensions the ingest path can actually decode. A file that does not appear
// here would only ever be offered and then rejected, so it is never listed.
constexpr const char* kMacroExtensions[] = {".macro", ".slc", ".json", ".csv", ".txt"};

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

} // namespace

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
    m_statusLabel->setScale(0.42f);
    m_statusLabel->setAnchorPoint(CCPoint(0.5f, 0.5f));
    m_mainLayer->addChildAtPosition(m_statusLabel, Anchor::Center, ccp(0.f, height * 0.5f - 40.f));

    // A plain CCMenu with hand-placed items. The engine's own menu layout helpers
    // would fight the fixed geometry here, and five static rows do not need a
    // layout engine.
    auto* menu = CCMenu::create();
    menu->setID("novabot.autofpcount/menu");
    m_mainLayer->addChildAtPosition(menu, Anchor::Center);

    struct Row {
        const char* caption;
        void (FpPopup::*handler)(CCObject*);
        float x;
        float y;
    };

    const Row rows[] = {
        {"Previous", &FpPopup::onPrevious, -74.f, 62.f},
        {"Next", &FpPopup::onNext, 74.f, 62.f},
        {"Reload", &FpPopup::onReload, 0.f, 12.f},
        {"Accelerated", &FpPopup::onAccelerated, 0.f, -44.f},
        {"Export", &FpPopup::onExport, 0.f, -100.f},
    };

    for (const Row& row : rows) {
        auto* item = CCMenuItemSpriteExtra::create(
            ButtonSprite::create(row.caption), this, menu_selector(row.handler));
        menu->addChild(item);
        item->setPosition(row.x, row.y);
    }

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

    const std::string name = m_files.empty()
                                 ? std::string("(no macros found)")
                                 : m_files[m_index].filename().string();

    const char* speed = speedName(GameSession::get().playback().speed());
    const std::string& diagnostics = GameSession::get().macroDiagnostics();

    std::string text = "Macro: " + name + "\n";
    text += std::string("Speed: ") + speed + "\n";
    text += diagnostics.empty() ? std::string("Gate: not loaded") : ("Gate: " + diagnostics);

    m_statusLabel->setString(text.c_str());
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

void FpPopup::onAccelerated(CCObject*) {
    auto& session = GameSession::get();
    const bool on = session.playback().speed() == PlaybackSpeed::Accelerated;
    session.setPlaybackSpeed(on ? PlaybackSpeed::Normal : PlaybackSpeed::Accelerated);
    refreshStatus();
}

void FpPopup::onExport(CCObject*) {
    std::string error;
    const bool ok = GameSession::get().exportRecording(
        Recorder::defaultExportPath(), MacroFormat::Unknown, &error);

    if (m_statusLabel == nullptr) return;
    m_statusLabel->setString(ok ? "Exported capture.macro" : ("Export failed: " + error).c_str());
}

} // namespace afpc