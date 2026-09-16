/*
 * What the buttons mean and what the panel shows as a result.
 *
 * This owns the Buttons driver, so nothing else has to know which pad does
 * what: pads become intents in action_of(), and screens use the intents.
 *
 *   home:  up/down  change the volume
 *          select   opens the menu
 *   menu:  up/down  move the cursor
 *          select   activates the item under it
 *          back     returns home
 *
 * A new screen is an enumerator and a draw_*(); a new menu entry is a row in
 * the table in ui.cpp and a case in activate().
 *
 * Threading: run() is a thread body blocking on button events, draw() runs on
 * whichever thread owns the panel, and nothing is locked between them. Every
 * shared field is one byte, so each is read and written atomically here and a
 * draw can at worst mix two fields from instants microseconds apart. Do not
 * add a shared field wider than a word without adding a lock with it.
 *
 * The UI owns the panel from Config::y down and touches nothing above it, so
 * the caller keeps the area above - which is also why no board header is
 * needed here.
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "ch.h"
#include "hal.h"

#include <pika/audio/DACSpeaker.h>
#include <pika/buttons.h>
#include <pika/lcd/ST75160.h>

namespace pika::ui {

class Ui {
public:

    /** @brief  Everything the UI needs from outside itself. */
    struct Config {
        pika::lcd::ST75160 *lcd;            /**< Panel to draw on.            */
        pika::audio::DACSpeaker *speaker;   /**< Volume is applied to this.   */
        int y;                              /**< Top of the area UI owns.     */
        pika::input::Buttons::Config buttons; /**< Wiring, from the board.    */
        void (*test_sound)();               /**< Menu item, null if none.     */
    };

    explicit Ui(const Config &cfg);

    /**
     * @brief   Starts the buttons and applies the initial volume.
     * @note    Call once, from a thread, after chSysInit().
     */
    void init();

    /** @brief  Button event loop. Never returns; give it its own thread. */
    void run();

    /** @brief  Draws the current screen; the panel's owner does the flush. */
    void draw();

    /**
     * @brief   True once if the screen changed since the last call.
     * @note    Clears the flag, so call it before draw(), never after - a
     *          change landing mid draw must survive to the next one. Clearing
     *          only when it was set keeps that true at any thread priority.
     */
    bool take_dirty() {
        if (!dirty_) { return false; }
        dirty_ = false;
        return true;
    }

private:

    /* 20 steps, so the display moves in 5% increments. */
    static constexpr uint8_t volume_steps = 20;

    /* Tracks a loudness, not a step number: gain 0.2, which under the taper
       lands nearest at step 15 (0.178). */
    static constexpr uint8_t volume_initial = 15;

    /* Constant dB per press, so every press is the same change in loudness. */
    static constexpr float volume_db_per_step = 3.0f;

    /* Below this the taper goes linear, see volume_gain(). */
    static constexpr uint8_t volume_knee_step = 2;

    enum class Screen : uint8_t { home, menu };

    /* What a press means, rather than which pad it came from. */
    enum class Action : uint8_t { none, up, down, select, back };

    static Action action_of(pika::input::Button b);
    static float volume_gain(uint8_t step);

    unsigned volume_percent() const;

    void handle(Action a);
    void activate();
    void adjust_volume(int delta);

    void draw_home();
    void draw_menu();

    Config cfg_;
    pika::input::Buttons buttons_;

    volatile bool dirty_;
    volatile uint8_t volume_step_;
    volatile Screen screen_;
    volatile uint8_t cursor_;
    volatile bool backlight_;
};

}  // namespace pika::ui
