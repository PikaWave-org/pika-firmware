#include "ch.h"
#include "hal.h"

#include "chprintf.h"

#include <pika/audio/DACSpeaker.h>
#include <pika/audio/PCMPlayer.h>
#include <pika/audio/sounds/meow.h>
#include <pika/buttons.h>
#include <pika/log.h>
#include <pika/lcd/ST75160.h>
#include <pika/ui.h>

/* The panel, wired up as the board header describes it. */
static const pika::lcd::ST75160::Config lcd_cfg = {&BOARD_LCD_I2C,       BOARD_LCD_I2C_ADDR, LINE_LCD_RST,
                                                   LINE_LCD_BKLT,        LINE_LCD_SCL,       LINE_LCD_SDA,
                                                   BOARD_LCD_I2C_PINMODE};

static pika::lcd::ST75160 lcd{lcd_cfg};

/* The speaker, likewise. */
static const pika::audio::DACSpeaker::Config spk_cfg = {&BOARD_SPK_DAC, &BOARD_SPK_TIMER, LINE_SPK_EN,
                                                        BOARD_SPK_SETTLE_MS};

static pika::audio::DACSpeaker spk{spk_cfg};
static pika::audio::PCMPlayer pcm_player;

/* BTN_PWR is the polled one: it shares EXTI channel 10 with BTN_UP and the
   channel serves one port at a time, see board.h. */
static const pika::input::Buttons::Config btn_cfg = {{{LINE_BTN_PWR, true},
                                                      {LINE_BTN_UP, false},
                                                      {LINE_BTN_DOWN, false},
                                                      {LINE_BTN_RIGHT, false},
                                                      {LINE_BTN_LEFT, false},
                                                      {LINE_BTN_PTT, false},
                                                      {LINE_BTN_LSN, false}},
                                                     TIME_MS2I(BOARD_BTN_DEBOUNCE_MS),
                                                     TIME_MS2I(BOARD_BTN_POLL_MS)};

/* The UI takes a plain function, so it needs to know nothing about players or
   sound assets. */
static void play_test_sound() {
    if (!pcm_player.play(spk, pika::audio::meow)) { LOG("spk: meow rejected"); }
}

/* The UI owns the panel below this; the title and beat counter above it stay
   this file's business. */
static constexpr int ui_y = 30;

static const pika::ui::Ui::Config ui_cfg = {&lcd, &spk, ui_y, btn_cfg, play_test_sound};

static pika::ui::Ui ui{ui_cfg};

/* At file scope so it can be read out of RAM over SWD; a local would live in
   a register and have no symbol. */
static volatile uint32_t flush_ms;

/*
 * Heartbeat: a debug UART line and a counter on the LCD once a second, plus
 * the UI whenever it changes.
 *
 * The loop ticks ten times a second but only flushes on a beat or a UI change.
 * A flush is one I2C transaction for the whole frame, so an unchanged image is
 * not worth one, and waiting a whole second after a keypress reads as a dead
 * button.
 *
 * The panel is flushed only here, so everything on it is drawn from this
 * thread.
 */
static constexpr unsigned heartbeat_tick_ms = 100;
static constexpr unsigned heartbeat_ticks_per_beat = 1000 / heartbeat_tick_ms;

static THD_WORKING_AREA(waHeartbeat, 2048);

static THD_FUNCTION(heartbeat, arg) {
    (void) arg;
    chRegSetThreadName("heartbeat");

    uint32_t beat = 0;
    unsigned tick = 0;

    while (true) {
        const bool beat_due = (tick == 0);

        /* Before the draw, never after, so a change arriving mid draw stays
           pending for the next tick. */
        const bool ui_moved = ui.take_dirty();

        /* Vop is an I2C command sharing the sequencing buffer with flush(),
           so the UI only records it and it is sent from here. */
        uint16_t vop;
        if (ui.take_contrast(vop) && !lcd.contrast(vop)) {
            LOG("lcd: contrast failed, i2c error 0x%08x", (unsigned) lcd.last_error());
        }

        if (beat_due) {
            LOG("heartbeat %u", (unsigned) beat);

            char line[24];
            chsnprintf(line, sizeof line, "beat %u", beat);
            lcd.rect(4, 20, 8 * 12, 8, false);
            lcd.text(4, 20, line);
        }

        if (beat_due || ui_moved) {
            ui.draw();

            systime_t start = chVTGetSystemTimeX();
            bool ok = lcd.flush();
            sysinterval_t took = chVTTimeElapsedSinceX(start);
            flush_ms = (uint32_t) TIME_I2MS(took);

            if (!ok) {
                LOG("lcd: flush failed, i2c error 0x%08x", (unsigned) lcd.last_error());
            } else if (beat == 0) {
                LOG("lcd: flush took %ums", (unsigned) flush_ms);
            }
        }

        if (beat_due) { beat++; }

        chThdSleepMilliseconds(heartbeat_tick_ms);
        tick = (tick + 1) % heartbeat_ticks_per_beat;
    }
}

/* The UI owns this thread: run() blocks on button events and never returns. */
static THD_WORKING_AREA(waUi, 1024);

static THD_FUNCTION(ui_reader, arg) {
    (void) arg;
    chRegSetThreadName("ui");

    ui.run();
}

int main() {
    halInit();
    chSysInit();

    pika::log_init();
    LOG("\r\n" BOARD_NAME " starting");

    lcd.backlight(true);
    palClearLine(LINE_MIC_EN);

    bool ok = lcd.init();
    LOG("lcd: init %s, i2c error 0x%08x", ok ? "ok" : "failed", (unsigned) lcd.last_error());

    if (ok) {
        lcd.clear();
        lcd.text(4, 6, BOARD_NAME);
        if (!lcd.flush()) {
            LOG("lcd: flush failed, i2c error 0x%08x", (unsigned) lcd.last_error());
        }
    }

    /* verify() runs with the amplifier muted: it catches a DMA path that moves
     nothing, which otherwise looks working until you put an ear to it. */
    bool spk_ok = spk.init();
    LOG("spk: init %s", spk_ok ? "ok" : "failed");

    chThdCreateStatic(waHeartbeat, sizeof(waHeartbeat), NORMALPRIO, heartbeat, nullptr);

    /* Applies the initial volume as well as starting the buttons, so it has to
     run before anything can be played. */
    ui.init();

    chThdCreateStatic(waUi, sizeof(waUi), NORMALPRIO, ui_reader, nullptr);

    while (true) {
        chThdSleepMilliseconds(2000);
    }
}
