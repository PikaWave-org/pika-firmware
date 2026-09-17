#include "ch.h"
#include "hal.h"

#include "chprintf.h"

#include <cmath>

#include <pika/audio/ADCMicrophone.h>
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

/*
 * The 32kHz sample clock, shared by the speaker and the microphone. They never
 * run together - the device either talks or listens - and the clock is what
 * makes that exclusive rather than merely intended.
 */
static pika::audio::SampleClock sample_clock{&BOARD_SAMPLE_TIMER};

/* The speaker, likewise. */
static const pika::audio::DACSpeaker::Config spk_cfg = {&BOARD_SPK_DAC, &sample_clock, LINE_SPK_EN,
                                                        BOARD_SPK_SETTLE_MS};

static pika::audio::DACSpeaker spk{spk_cfg};
static pika::audio::PCMPlayer pcm_player;

/* The microphone, on the same clock. */
static const pika::audio::ADCMicrophone::Config mic_cfg = {&BOARD_MIC_ADC, &sample_clock, LINE_MIC_SHDN,
                                                           BOARD_MIC_ADC_CHANNEL, BOARD_MIC_SETTLE_MS};

static pika::audio::ADCMicrophone mic{mic_cfg};

/*
 * Bring-up instrumentation for the microphone, and nothing more: it measures
 * each 8ms block and throws it away.
 *
 * The counters are deliberately non-static globals so that they can be read
 * out of RAM over SWD by name. That is not a stylistic choice - the panel's
 * flush wedges the heartbeat thread a few seconds after boot, so anything
 * logged after that is never seen, and a debugger is the only way to watch
 * this run for longer than the debug UART survives.
 */
uint32_t mic_blocks;   /* 8ms blocks captured, ~125 a second while running. */
uint32_t mic_dc;       /* The driver's DC estimate, in raw ADC codes.       */
uint32_t mic_peak;     /* Largest magnitude in the last block.              */
uint32_t mic_rms;      /* RMS of the last block.                            */
uint32_t mic_peak_max; /* Largest magnitude since the capture started.      */
uint32_t mic_hold;     /* Peak with a decay, which is what the bar shows.   */

class MicLevel : public pika::audio::AudioConsumer {
public:
    /* Runs in the DMA interrupt: arithmetic only, no logging and no blocking.
       The sum needs 64 bits - 256 squares of up to 32768 overflow 32. */
    void take_audio_buffer(std::span<const int16_t> buf) override {

        uint32_t peak = 0;
        uint64_t sum = 0;

        for (int16_t s: buf) {
            const uint32_t mag = (uint32_t) (s < 0 ? -(int32_t) s : (int32_t) s);

            if (mag > peak) { peak = mag; }
            sum += (uint64_t) mag * mag;
        }

        mic_peak = peak;
        mic_rms = buf.empty() ? 0U : (uint32_t) sqrtf((float) (sum / buf.size()));

        if (peak > mic_peak_max) { mic_peak_max = peak; }

        /*
         * Peak hold, rising instantly and falling about 3% a block. The panel
         * is redrawn at 5Hz against 125 blocks a second, so the bare peak of
         * whichever 8ms block the draw happened to land on is noise; this
         * gives the bar a quarter second of memory and makes it readable.
         */
        if (peak > mic_hold) {
            mic_hold = peak;
        } else {
            mic_hold -= mic_hold >> 5;
        }

        mic_dc = mic.dc_level();
        mic_blocks++;
    }
};

static MicLevel mic_level;

/*
 * True once mic.init() has succeeded, so the heartbeat knows there is a
 * microphone worth starting.
 */
static bool mic_ready;

/*
 * A sound the UI has asked for.
 *
 * The UI cannot simply play it: the speaker and the microphone share one
 * clock, so something has to stop the capture first, and a caller that did
 * that from the UI thread would race the heartbeat restarting it. Recording
 * the request and letting the heartbeat act on it makes the arbitration
 * single threaded - the same shape the panel already uses for contrast.
 */
static volatile bool sound_requested;

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

/* The UI takes a plain function, so it needs to know nothing about players,
   sound assets, or who is holding the sample clock. */
static void play_test_sound() { sound_requested = true; }

/* The UI owns the panel below this; the title and beat counter above it stay
   this file's business. */
static constexpr int ui_y = 30;

static const pika::ui::Ui::Config ui_cfg = {&lcd, &spk, ui_y, btn_cfg, play_test_sound};

static pika::ui::Ui ui{ui_cfg};

/* At file scope so it can be read out of RAM over SWD; a local would live in
   a register and have no symbol. */
static volatile uint32_t flush_ms;

/*
 * The microphone level block, at the bottom of the panel below everything the
 * UI draws: its home screen ends at the volume bar (y = 58) and its menu at
 * the fourth row (y = 68), so this clears both.
 */
static constexpr int mic_text_y = 74;
static constexpr int mic_bar_y = 84;
static constexpr int mic_bar_x = 4;
static constexpr int mic_bar_w = pika::lcd::ST75160::width - 8;
static constexpr int mic_bar_h = 12;
static constexpr int mic_bar_inset = 2;

static_assert(mic_bar_y + mic_bar_h <= pika::lcd::ST75160::height, "mic bar runs off the bottom of the panel");
static_assert(mic_text_y >= 70, "mic block overlaps the UI's fourth menu row");

/*
 * The bottom of the dB scale, and the amplitude it stands for. Below that
 * amplitude - which includes zero, whose logarithm is minus infinity and
 * converts to nothing sensible - a reading is just the floor.
 *
 * GCC folds __builtin_powf over constants, so the threshold follows the floor
 * at compile time instead of being a second number to keep in step with it.
 */
static constexpr float db_floor = -100.0f;
static constexpr float db_floor_mag = __builtin_powf(10.0f, db_floor / 20.0f);

/* An amplitude as dB, where 1.0 is 0dB and so the readings are negative. */
static float to_db(float mag) {

    const float db = mag > db_floor_mag ? 20.0f * log10f(mag) : db_floor;

    return db > 0.0f ? 0.0f : db;
}

/* The level counters are magnitudes of signed 16 bit samples. */
static constexpr float mic_full_scale = 32767.0f;

/*
 * The bottom of the meter, which is not the bottom of the scale: one ADC code
 * is -66dB here, quieter than the front end can resolve, so -70dB leaves a
 * quiet room's rms of 20 to 40 codes just off the end and speech a hand's
 * width away, peaking near -18dB, three quarters along. Both the bar and the
 * number bottom out here, and two digits is also all the line has room for.
 */
static constexpr float mic_meter_db = -70.0f;

/*
 * Draws the level block. Called from the heartbeat, which owns the panel -
 * the UI thread must not draw or send, for the reason ui.h gives.
 *
 * The peak is read once into a local because the DMA callback rewrites it
 * every 8ms, and the text and the bar have to agree with each other. The
 * capture callback keeps recording plain magnitudes: the logarithms are two a
 * redraw here against 125 blocks a second in the interrupt, and a linear peak
 * hold is what decays by a shift.
 */
static void draw_mic() {

    const float hold_db = fmaxf(to_db((float) mic_hold / mic_full_scale), mic_meter_db);
    const float rms_db = fmaxf(to_db((float) mic_rms / mic_full_scale), mic_meter_db);

    /* chprintf has no floating point, and whole dB is as fine as this reads
       anyway. No space before the numbers: they carry their own sign, and the
       widest line this can produce is 19 characters, which is the whole width
       of the panel in the 8x8 font. */
    char line[24];
    chsnprintf(line, sizeof line, mic.capturing() ? "mic rms%d pk%d dB" : "mic off", (int) lrintf(rms_db),
               (int) lrintf(hold_db));

    lcd.rect(4, mic_text_y, pika::lcd::ST75160::width - 8, 8, false);
    lcd.text(4, mic_text_y, line);

    lcd.rect(mic_bar_x, mic_bar_y, mic_bar_w, mic_bar_h, false);
    lcd.frame(mic_bar_x, mic_bar_y, mic_bar_w, mic_bar_h, true);

    const int inner = mic_bar_w - 2 * mic_bar_inset;
    const int fill = (int) ((float) inner * (hold_db - mic_meter_db) / -mic_meter_db);

    if (fill > 0) {
        lcd.rect(mic_bar_x + mic_bar_inset, mic_bar_y + mic_bar_inset, fill, mic_bar_h - 2 * mic_bar_inset, true);
    }
}

/*
 * Hands the sample clock between the speaker and the microphone.
 *
 * Only this thread does it, so there is no window where one has let go and the
 * other has not yet claimed. The microphone runs whenever nothing else wants
 * the clock, which is what keeps the level block live.
 */
static void serve_audio() {

    if (sound_requested) {
        sound_requested = false;

        if (mic.capturing()) { mic.stop_capture(); }

        if (!pcm_player.play(spk, pika::audio::meow)) { LOG("spk: meow rejected"); }
        return;
    }

    if (mic_ready && !mic.capturing() && !spk.busy() && !mic.start_capture(mic_level)) {
        LOG("mic: capture rejected, error 0x%08x", (unsigned) mic.last_error());
        mic_ready = false;/* do not retry ten times a second */
    }
}

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

        /* Before the drawing below, so a capture starting this tick is already
           reflected in the level block. */
        serve_audio();

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

        /*
         * A level meter that moved once a second would not read as a level
         * meter, so while capturing the block is refreshed every other tick.
         * A flush is 53ms of I2C, but the thread sleeps on the DMA for all of
         * it, so 5Hz costs bus bandwidth rather than CPU.
         */
        const bool mic_due = mic.capturing() && (tick % 2U) == 0U;

        if (beat_due || ui_moved || mic_due) {
            ui.draw();
            draw_mic();

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

    /* Nothing is captured here. The heartbeat starts the microphone once it is
       running and hands the clock back whenever the speaker wants it, so boot
       is not held up waiting for a preamp to settle. */
    mic_ready = mic.init();
    LOG("mic: init %s", mic_ready ? "ok" : "failed");

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
