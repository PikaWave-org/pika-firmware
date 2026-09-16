#include "ch.h"
#include "hal.h"

#include "chprintf.h"

#include <pika/audio/DACSpeaker.h>
#include <pika/audio/PCMPlayer.h>
#include <pika/audio/sounds/meow.h>
#include <pika/buttons.h>
#include <pika/log.h>
#include <pika/lcd/ST75160.h>
#include <pika/ublox.h>
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

/* The GNSS receiver, at one navigation solution per second on its UART1. It
   reports to the debug UART only - nothing of it is on the panel yet. */
static const pika::gnss::Ublox::Config gnss_cfg = {&BOARD_GNSS_SERIAL, BOARD_GNSS_BAUD_RUN, 1000U, 1U};

static pika::gnss::Ublox gnss{gnss_cfg};

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
 * Formats a 1e-7 degree coordinate as plain decimal degrees. chprintf() has
 * no floating point support in this build, and the sign has to be taken off
 * before the split so that -0.5 degrees does not come out as "-0.-5000000".
 */
static void format_deg(char *out, size_t len, int32_t deg_1e7) {

    bool neg = deg_1e7 < 0;
    uint32_t mag = (uint32_t) (neg ? -(int64_t) deg_1e7 : (int64_t) deg_1e7);

    chsnprintf(out, len, "%s%u.%07u", neg ? "-" : "", (unsigned) (mag / 10000000U), (unsigned) (mag % 10000000U));
}

/* Antenna supervisor state, abbreviated to fit beside the jamming figures. */
static const char *antenna_text(uint8_t status) {

    switch (static_cast<pika::gnss::AntennaStatus>(status)) {
        case pika::gnss::AntennaStatus::INIT:
            return "init";
        case pika::gnss::AntennaStatus::DONTKNOW:
            return "unk";
        case pika::gnss::AntennaStatus::OK:
            return "ok";
        case pika::gnss::AntennaStatus::SHORT:
            return "shrt";
        case pika::gnss::AntennaStatus::OPEN:
            return "open";
        default:
            return "?";
    }
}

/*
 * Reports the last solution the driver has in memory.
 *
 * The frame and error counts go out alongside it because the two failure
 * modes look identical otherwise: a receiver with no sky view and a receiver
 * that is not talking at all both report fixType 0 and numSV 0.
 */
static void log_gnss() {

    pika::gnss::msg_rx_nav_pvt_s pvt;
    uint32_t age_ms = 0;

    if (!gnss.nav_pvt(pvt, &age_ms)) {
        LOG("gnss: no solution yet, frames %u errors %u", (unsigned) gnss.frames(), (unsigned) gnss.errors());
        return;
    }

    /* A "!" marks a solution the receiver itself does not trust. */
    bool fix_ok = (pvt.flags & 0x01U) != 0U;

    char lat[16], lon[16];
    format_deg(lat, sizeof lat, pvt.lat);
    format_deg(lon, sizeof lon, pvt.lon);

    LOG("gnss: age=%us iTOW=%u fix=%u%s sv=%u lat=%s lon=%s alt=%dm hacc=%um pdop=%u.%02u frames=%u errors=%u",
        (unsigned) (age_ms / 1000U), (unsigned) (pvt.iTOW / 1000), (unsigned) pvt.fixType, fix_ok ? "" : "!",
        (unsigned) pvt.numSV, lat, lon, (int) (pvt.hMSL / 1000), (unsigned) (pvt.hAcc / 1000U),
        (unsigned) (pvt.pDOP / 100U), (unsigned) (pvt.pDOP % 100U), (unsigned) gnss.frames(), (unsigned) gnss.errors());

    /* Date, time and the UTC offset all have to be resolved before the
       timestamp is worth printing. */
    if ((pvt.valid & 0x07U) == 0x07U) {
        LOG("gnss: utc %04u-%02u-%02u %02u:%02u:%02u", (unsigned) pvt.year, (unsigned) pvt.month, (unsigned) pvt.day,
            (unsigned) pvt.hour, (unsigned) pvt.min, (unsigned) pvt.sec);
    }
}

/*
 * Reports the last MON-HW the driver has in memory, once per beat - which is
 * one line per message, since MON-HW is enabled at one per navigation epoch
 * and the epoch is a second.
 *
 * jamInd and the jamming state are logged for completeness but do not carry
 * information yet: the interference monitor is left disabled, so the receiver
 * reports 0 and "unknown" for both. Enabling it is a CFG-ITFM away.
 */
static void log_mon_hw() {

    pika::gnss::msg_rx_mon_hw_s hw;
    uint32_t age_ms = 0;

    if (!gnss.mon_hw(hw, &age_ms)) {
        LOG("gnss: no hw status yet");
        return;
    }

    /* age leads the line deliberately. Everything after it stays plausible
       forever once the link dies - noise and AGC do not decay, and MON-HW has
       no iTOW to visibly stop advancing - so the age is the only thing on the
       line that says whether any of it is still true. */
    LOG("gnss: hw age=%us noise=%u agc=%u jam=%u jamstate=%u ant=%s apwr=%u flags=0x%02x", (unsigned) (age_ms / 1000U),
        (unsigned) hw.noisePerMS, (unsigned) hw.agcCnt, (unsigned) hw.jamInd, (unsigned) pika::gnss::jamming_state(hw),
        antenna_text(hw.aStatus), (unsigned) hw.aPower, (unsigned) hw.flags);
}

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

/* The GNSS driver owns this thread: run() configures the receiver, sweeping
   the baud rates to find it, and then reads forever. */
static THD_WORKING_AREA(waGnss, 2048);

static THD_FUNCTION(gnss_reader, arg) {
    (void) arg;
    chRegSetThreadName("gnss");

    gnss.run();
}

/*
 * Reporting gets its own thread rather than riding the heartbeat.
 *
 * The heartbeat thread blocks forever in lcd.flush() within a few beats on
 * most boots - see memory/lcd-flush-hangs-after-few-beats.md - so logging
 * from there took the receiver's measurements down with the panel, and a
 * capture that stopped could not be read as either the LCD hanging or the
 * link dying. Here nothing the panel does can starve it.
 *
 * Polling the accessors, rather than logging from the driver's message
 * handler, is what keeps a dead link visible: if no message ever arrives this
 * still prints a line a second with frames() and errors() standing still,
 * which is the signature of a link that has stopped. Logging on arrival would
 * simply go quiet, which is the same ambiguity in a different place.
 */
static THD_WORKING_AREA(waGnssLog, 2048);

static THD_FUNCTION(gnss_logger, arg) {
    (void) arg;
    chRegSetThreadName("gnss-log");

    while (true) {
        log_gnss();
        log_mon_hw();
        chThdSleepMilliseconds(1000);
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

    /* Finding the receiver means sweeping the baud rates, which takes seconds
       and has no business holding up the rest of the boot. */
    chThdCreateStatic(waGnss, sizeof(waGnss), NORMALPRIO, gnss_reader, nullptr);
    chThdCreateStatic(waGnssLog, sizeof(waGnssLog), NORMALPRIO, gnss_logger, nullptr);

    /* Applies the initial volume as well as starting the buttons, so it has to
     run before anything can be played. */
    ui.init();

    chThdCreateStatic(waUi, sizeof(waUi), NORMALPRIO, ui_reader, nullptr);

    while (true) {
        chThdSleepMilliseconds(2000);
    }
}
