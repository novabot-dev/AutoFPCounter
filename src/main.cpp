#include <Geode/Geode.hpp>

#include <Geode/binding/ButtonSprite.hpp>
#include <Geode/binding/CCMenu.hpp>
#include <Geode/binding/CCMenuItemSpriteExtra.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/loader/Mod.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>

#include "core/Constants.hpp"
#include "game/GameSession.hpp"
#include "ui/FpPopup.hpp"

using namespace geode::prelude;
using namespace afpc;

// ---------------------------------------------------------------------------
// Load guard. Refuses to initialise on vanilla Geometry Dash.
// ---------------------------------------------------------------------------
bool checkGDR(bool isGeode) {
    if (!isGeode) {
        log::error("AutoFPCount requires the Geode loader and will not run on vanilla "
                   "Geometry Dash");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// PlayLayer hooks
//
// update() is the tick boundary. Everything the mod knows is sampled here, in
// this order:
//
//   preEngineTick   inject this tick's scheduled macro inputs
//   <engine update> simulate the tick
//   postEngineTick  sample the interaction state, classify any clicks
//
// Inputs are queued before the engine consumes its input queue, so a press lands
// on the tick it was recorded for rather than one tick late.
// ---------------------------------------------------------------------------
class $modify(PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        // Replay playback is not a live attempt; do not attach there.
        if (useReplay) return true;

        GameSession::get().attach(this);
        return true;
    }

    void onExit() {
        GameSession::get().detach();
        PlayLayer::onExit();
    }

    void update(float dt) {
        auto& session = GameSession::get();

        // Order matters and is not interchangeable. Every simulated tick is
        // bracketed by pre/post, and the physics step for a tick must sit between
        // its own two hooks - otherwise the inputs injected for tick N would be
        // consumed by a later tick's physics, and the driver's ticks would run
        // before the tick they are supposed to follow.
        session.preEngineTick(this);
        PlayLayer::update(dt);
        session.postEngineTick(this);

        // Only now, with this frame's tick fully closed, is it safe to fast-forward.
        driveExtraTicks(dt, session);
    }

    void postUpdate(float dt) {
        GameSession::get().postFrame(this, dt);
        PlayLayer::postUpdate(dt);
    }

    // Accelerated timescale driver.
    //
    // Calls PlayLayer::update() extra times after the frame's own tick has been
    // closed, so a macro replays faster than real time. Those calls appear to the
    // rest of the mod as ordinary ticks - each one is bracketed by the same
    // pre/post pair - which is what keeps the tick index a single source of truth.
    // Three properties matter:
    //
    //   * Reentrancy is impossible. The latch lives in GameSession and is held for
    //     the whole nested loop, so a nested update() - which the engine can
    //     produce through its own scheduling - can never re-enter the driver and
    //     recurse.
    //
    //   * The loop is hard bounded by kMaxStepsPerFrame. A single frame can never
    //     stall the game regardless of what dt clamping allows.
    //
    //   * Nothing is skipped. Hitting the cap leaves the remainder in the
    //     FixedClock, so the work is deferred to the next frame instead of
    //     dropped. Playback therefore never advances its cursor past a tick that
    //     was not actually simulated.
    //
    // The driven ticks use a fixed kTickSeconds dt while the trailing call keeps
    // the engine's real dt. That mix is inherent to "step the engine faster than
    // wall time" and is why the mode is opt-in.
    //
    // Note the latch deliberately is NOT a member of this class. A Geode $modify
    // struct is cast onto the engine's own allocation for member-offset purposes,
    // so a plain data member here would live past the end of the real PlayLayer.
    // Adding storage to a modified class requires Geode's `struct Fields`.
    void driveExtraTicks(float dt, GameSession& session) {
        if (session.playback().speed() != PlaybackSpeed::Accelerated) return;
        if (!session.attemptActive()) return;
        if (!session.driverEnter()) return; // a driver loop is already running

        const int extra = session.playback().accumulateExtraTicks(dt);
        for (int i = 0; i < extra; ++i) {
            session.preEngineTick(this);
            PlayLayer::update(static_cast<float>(kTickSeconds));
            session.postEngineTick(this);
        }

        session.driverExit();
    }
};

// ---------------------------------------------------------------------------
// Raw input hook.
//
// handleButton is the engine's single input entry point - keyboard, touch, and
// the macro systems all funnel through it. Intercepting here gives the recorder
// exactly the events the engine itself acts on, at the cost of one predicated
// branch per input event. No polling, no per-frame scan.
//
// This hook lives on the base class because handleButton is not overridden by
// PlayLayer, so one hook covers every gameplay layer.
// ---------------------------------------------------------------------------
class $modify(GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);

        // handleButton also fires for editor sessions, so resolve the live
        // PlayLayer rather than casting blindly. This runs a handful of times per
        // attempt, so the lookup cost is irrelevant next to the correctness.
        PlayLayer* layer = PlayLayer::get();
        if (layer != nullptr) {
            GameSession::get().onRawButton(layer, down, button, isPlayer1);
        }
    }
};

// ---------------------------------------------------------------------------
// Pause menu entry point.
//
// A single "FP" squircle appended to the pause menu's button column, which opens
// the control popup. customSetup() is used rather than init() because the menu
// this button is added to does not exist until after the engine's own setup has
// run.
//
// The two-argument $modify form is required here, unlike the two hooks above: the
// one-argument form produces an anonymous class, and menu_selector() needs a
// nameable class to take the address of an incoming handler.
// ---------------------------------------------------------------------------
class $modify(FpPauseHook, PauseLayer) {
    $override
    void customSetup() {
        PauseLayer::customSetup();

        auto* menu = static_cast<CCMenu*>(this->getChildByID("right-button-menu"));
        if (menu == nullptr) return;

        // customSetup can run again on a re-created layer; never stack buttons.
        if (menu->getChildByID("novabot.autofpcount/fp-button") != nullptr) return;

        auto* button = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("FP"),
            this,
            menu_selector(FpPauseHook::onFpPressed)
        );
        button->setID("novabot.autofpcount/fp-button");
        menu->addChild(button);

        // Let the engine re-space the column now that it has one more child.
        menu->updateLayout();
    }

    void onFpPressed(CCObject*) {
        if (auto* popup = FpPopup::create()) popup->show();
    }
};