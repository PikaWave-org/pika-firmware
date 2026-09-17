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
 *   edit:  up/down  change the value under the cursor
 *          select   leaves the value where it is
 *          back     the same - there is nothing to cancel, every change has
 *                   already been applied and seen
 *   rec:   select   records for as long as it is held, see record_held()
 *          back     returns to the menu
 *
 * Editing is a mode of the menu rather than a screen of its own: the rows stay
 * where they are and only the value's rendering changes, so the eye does not
 * have to find the setting again after every press.
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

        /**< The record screen's status line, null if none. Called from draw(),
             so it runs on the panel's thread and may return a static buffer. */
        const char *(*record_status)();
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

    /**
     * @brief   True once if the contrast needs sending to the panel, in
     *          which case @p vop receives the value to send.
     * @note    Like take_dirty(), for the same reason: the command is an I2C
     *          transaction sharing the sequencing buffer with flush(), so the
     *          UI only records the change and the panel's owner applies it.
     */
    bool take_contrast(uint16_t &vop);

    /**
     * @brief   True while the user is holding the control that records.
     * @note    An intent rather than a pad, like action_of()'s mapping: which
     *          button it is stays this class's business. Polled rather than
     *          delivered, because the Buttons driver broadcasts presses only -
     *          there is no release event and no long press to wait on, so the
     *          caller has to ask on whatever tick it already has.
     * @note    Not const: the first release after the screen opens is a state
     *          change, see the gate in the implementation.
     */
    [[nodiscard]] bool record_held();

    /**
     * @brief   The first panel row below what the current screen occupies.
     * @note    For a caller drawing under the UI: it says whether there is
     *          still room, so adding a menu row moves this rather than
     *          silently overwriting whatever was below.
     */
    [[nodiscard]] int bottom_y() const;

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

    /* Contrast as the panel's Vop word, where V0 = 3.6 + vop * 0.04 volts.
       150..250 is 9.6V to 13.6V, a spread wide enough to go visibly light and
       visibly dark while staying well inside what the panel is rated for. */
    static constexpr uint16_t contrast_vop_min = 150U;
    static constexpr uint16_t contrast_vop_step = 10U;

    /* 10 steps of 0.4V, so 11 values. */
    static constexpr uint8_t contrast_steps = 10U;

    /* Vop 200, which is what init() programs: changing one without the other
       would make the menu disagree with the panel until the first press. */
    static constexpr uint8_t contrast_initial = 5U;

    enum class Screen : uint8_t { home, menu, record };

    /* What a press means, rather than which pad it came from. */
    enum class Action : uint8_t { none, up, down, select, back };

    static Action action_of(pika::input::Button b);
    static float volume_gain(uint8_t step);
    static uint16_t contrast_vop(uint8_t step);

    unsigned volume_percent() const;
    unsigned contrast_tenths() const;

    void handle(Action a);
    void activate();
    void adjust_volume(int delta);
    void adjust_contrast(int delta);

    void draw_home();
    void draw_menu();
    void draw_record();

    Config cfg_;
    pika::input::Buttons buttons_;

    volatile bool dirty_;
    volatile uint8_t volume_step_;
    volatile Screen screen_;
    volatile uint8_t cursor_;
    volatile bool backlight_;
    volatile uint8_t contrast_step_;
    volatile bool contrast_pending_;
    volatile bool editing_;
    volatile bool record_gate_;
};

}  // namespace pika::ui
