#include "ch.h"
#include "hal.h"

#include "chprintf.h"

#include <algorithm>
#include <cmath>

#include <pika/audio/ADCMicrophone.h>
#include <pika/audio/DACSpeaker.h>
#include <pika/audio/MelpRecorder.h>
#include <pika/audio/MicLevel.h>
#include <pika/audio/PCMPlayer.h>
#include <pika/audio/sounds/meow.h>
#include <pika/audio/tone_generator.h>
#include <pika/buttons.h>
#include <pika/lcd/ST75160.h>
#include <pika/lcd/images/pika_logo.h>
#include <pika/log.h>
#include <pika/ublox.h>
#include <pika/ui.h>

/* The panel, wired up as the board header describes it. */
static const pika::lcd::ST75160::Config lcd_cfg = {&BOARD_LCD_I2C, BOARD_LCD_I2C_ADDR, LINE_LCD_RST,
                                                   LINE_LCD_SCL,   LINE_LCD_SDA,       BOARD_LCD_I2C_PINMODE};

static pika::lcd::ST75160 lcd{lcd_cfg};

/*
 * The backlight: a LED on TIM1_CH1, 10kHz from a 10MHz counter, so one period
 * is 1000 ticks and the duty is the brightness. The counter frequency has to
 * divide the timer clock exactly - 260MHz / 10MHz = 26 here - or pwmStart()
 * asserts. Channels other than the backlight's stay PWM_OUTPUT_DISABLED, which
 * is what the zeroed entries mean.
 */
static constexpr uint32_t bklt_pwm_hz = 10000000U;
static constexpr pwmcnt_t bklt_period = bklt_pwm_hz / 10000U;

static PWMConfig bklt_pwm_cfg = {bklt_pwm_hz, bklt_period, nullptr, {}, 0U, 0U, 0U};

/*
 * The 32kHz sample clock, shared by the speaker and the microphone.
 */
static pika::audio::SampleClock sample_clock{&BOARD_SAMPLE_TIMER};

static const pika::audio::DACSpeaker::Config spk_cfg = {&BOARD_SPK_DAC, &sample_clock, LINE_SPK_EN,
                                                        BOARD_SPK_SETTLE_MS};
static pika::audio::DACSpeaker spk{spk_cfg};

static const pika::audio::ADCMicrophone::Config mic_cfg = {&BOARD_MIC_ADC, &sample_clock, LINE_MIC_SHDN,
                                                           BOARD_MIC_ADC_CHANNEL, BOARD_MIC_SETTLE_MS};
static pika::audio::ADCMicrophone mic{mic_cfg};

static pika::audio::PCMPlayer pcm_player;
static pika::audio::ToneGenerator tone_gen;

/*
 * The recording: a MELPe 2400 bitstream, encoded as it is captured and decoded
 * on the way back out, so no raw samples are stored. A minute costs 18.7KB
 * where four seconds of PCM used to cost 250KB.
 *
 * It sits behind the level meter, which keeps running through a recording, so
 * starting one does not touch the capture.
 */
static pika::audio::MelpRecorder melp_rec;
static THD_WORKING_AREA(waMelp, 8192);

static pika::audio::MicLevel mic_level;

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

/* The heartbeat's tick, up here because the recorder counts in them. */
static constexpr unsigned heartbeat_tick_ms = 100;

/*
 * The pause between the button coming up and the replay starting.
 *
 * It is what the feature asks for, and it is also what makes handing the
 * buffer to the player safe: an interrupt already past the arm check when the
 * heartbeat clears it appends one more block, and a second is 125 blocks of
 * margin on that. Anyone shortening this should know it is not only cosmetic.
 */
static constexpr unsigned replay_delay_ticks = 1000U / heartbeat_tick_ms;

/* The markers around the replay: an octave apart so which one you are hearing
   takes no thought, and the closing one longer because an ending reads better
   as a longer note. */
static constexpr float beep_in_hz = 800.0f;
static constexpr float beep_in_s = 0.1f;
static constexpr float beep_out_hz = 600.0f;
static constexpr float beep_out_s = 0.1f;
static constexpr float beep_gain = 0.3f;

/*
 * The record-and-replay sequence, stepped once per heartbeat tick.
 *
 * It lives on that thread because serve_audio() is the sole arbitrator of the
 * sample clock, and a second thread sequencing beeps with plain sleeps would
 * be exactly the race that arrangement exists to prevent. The states are the
 * waits: nothing here blocks, so the panel and the meter keep running through
 * a recording and a replay.
 *
 * These two are plain where everything around them is volatile, because only
 * the heartbeat ever touches them.
 */
enum class Rec : uint8_t { idle, recording, waiting, beep_in, replay, beep_out };

static Rec rec_state;
static unsigned rec_ticks;

/* Volume to put back after the replay, see serve_record(). */
static float rec_volume;

/* Tenths of a second of recording, for the screen. Counted in codec frames of
   22.5ms now, not in samples: nothing stores samples any more. */
static unsigned record_tenths() { return (unsigned) melp_rec.tenths(); }

/*
 * The record screen's second row. Called from the UI's draw(), which runs on
 * the heartbeat, so this reads rec_state from the thread that owns it and the
 * static buffer cannot be overwritten under the caller.
 */
static const char *record_status() {

    static char text[20];

    if (!mic_ready) {
        return "no mic";
    }

    switch (rec_state) {
        case Rec::recording:
            /* An overrun outranks FULL: running out of buffer is by design, a
               dropped frame is a missed deadline. */
            chsnprintf(text, sizeof text,
                       melp_rec.overruns() != 0U ? "rec %u.%us !ovr"
                                                 : (melp_rec.full() ? "rec %u.%us FULL" : "rec %u.%us"),
                       record_tenths() / 10U, record_tenths() % 10U);
            break;

        case Rec::waiting:
            chsnprintf(text, sizeof text, "replay in %u.%us", rec_ticks / 10U, rec_ticks % 10U);
            break;

        case Rec::idle:
            if (melp_rec.frames() == 0U) {
                return "ready";
            }

            chsnprintf(text, sizeof text, "last %u.%us", record_tenths() / 10U, record_tenths() % 10U);
            break;

        default:
            chsnprintf(text, sizeof text, "replay %u.%us", record_tenths() / 10U, record_tenths() % 10U);
            break;
    }

    return text;
}

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

/* How long the logo stays up at boot, see main(). */
static constexpr uint32_t splash_ms = 2000U;

static_assert(pika::lcd::pika_logo_width <= pika::lcd::ST75160::width &&
                      pika::lcd::pika_logo_height <= pika::lcd::ST75160::height,
              "the splash image is larger than the panel");

/* The UI owns the panel below this; the title and beat counter above it stay
   this file's business. */
static constexpr int ui_y = 30;

static const pika::ui::Ui::Config ui_cfg = {
        &lcd, &spk, ui_y, btn_cfg, play_test_sound, record_status, &BOARD_LCD_BKLT_PWM, BOARD_LCD_BKLT_PWM_CHANNEL};

static pika::ui::Ui ui{ui_cfg};

/* At file scope so it can be read out of RAM over SWD; a local would live in
   a register and have no symbol. */
static volatile uint32_t flush_ms;

/*
 * The microphone level block, at the bottom of the panel below what the UI
 * draws, and only on the record screen - it is part of recording, not a
 * permanent readout. The heartbeat also asks the UI where it ended rather than
 * assuming there is room, so a record screen that grows a row breaks loudly
 * instead of being quietly overdrawn.
 */
static constexpr int mic_text_y = 74;
static constexpr int mic_bar_y = 84;
static constexpr int mic_bar_x = 4;
static constexpr int mic_bar_w = pika::lcd::ST75160::width - 8;
static constexpr int mic_bar_h = 12;
static constexpr int mic_bar_inset = 2;

static_assert(mic_bar_y + mic_bar_h <= pika::lcd::ST75160::height, "mic bar runs off the bottom of the panel");

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
 * The readings are taken once into locals because the DMA callback rewrites
 * them every 8ms, and the text and the bar have to agree with each other.
 */
static void draw_mic() {

    const float peak_db = fmaxf(mic_level.peak_db(), mic_meter_db);

    /* Whole dB is as fine as this reads. No space before the numbers: they
       carry their own sign, and the widest line this can produce is 19
       characters, which is the whole width of the panel in the 8x8 font. */
    char line[24];
    chsnprintf(line, sizeof line, mic.capturing() ? "mic %.1f dB" : "mic off", peak_db);

    lcd.rect<false>(4, mic_text_y, pika::lcd::ST75160::width - 8, 8);
    lcd.text(4, mic_text_y, line);

    lcd.rect<false>(mic_bar_x, mic_bar_y, mic_bar_w, mic_bar_h);
    lcd.frame<true>(mic_bar_x, mic_bar_y, mic_bar_w, mic_bar_h);

    const int inner = mic_bar_w - 2 * mic_bar_inset;
    const int fill = (int) ((float) inner * (peak_db - mic_meter_db) / -mic_meter_db);

    if (fill > 0) {
        lcd.rect<true>(mic_bar_x + mic_bar_inset, mic_bar_y + mic_bar_inset, fill, mic_bar_h - 2 * mic_bar_inset);
    }
}

/*
 * Steps the record-and-replay sequence, and says whether it is using the audio
 * path this tick.
 *
 * Every state that waits for a sound to finish waits on spk.busy() rather than
 * on having started one, so a start the speaker refused costs a beep and not
 * the sequence: the next tick finds the speaker idle and moves on. tone()
 * cannot report a refusal at all, which is why that matters.
 */
static bool serve_record() {

    switch (rec_state) {
        case Rec::idle:
            /* Nothing to record into if the microphone is not running - that is
               the meow holding the clock, and the hold simply takes effect on
               the first tick after it finishes. */
            if (!ui.record_held() || !mic.capturing()) {
                return false;
            }

            melp_rec.start_record();
            rec_state = Rec::recording;
            LOG("rec: recording");
            break;

        case Rec::recording:
            if (ui.record_held() && mic.capturing()) {
                break;
            }

            /* The tail is still in flight, so whether there is anything to
               replay is not knowable here; Rec::waiting settles it. */
            melp_rec.stop_record();

            rec_ticks = replay_delay_ticks;
            rec_state = Rec::waiting;
            break;

        case Rec::waiting:
            /* The microphone keeps running through the pause, so the meter
               stays live; it is handed over only when there is a sound to
               make. */
            if (rec_ticks > 0U) {
                rec_ticks--;
                break;
            }

            /* The delay above covers an interrupt mid-block; the codec thread
               holding up to eight queued frames is a second hazard it does
               not, so wait for it rather than assume. */
            if (!melp_rec.idle()) {
                break;
            }

            LOG("rec: %u frames, %u.%us, %u bytes, %u overruns", (unsigned) melp_rec.frames(),
                record_tenths() / 10U, record_tenths() % 10U, (unsigned) melp_rec.bytes(),
                (unsigned) melp_rec.overruns());

            if (melp_rec.frames() == 0U) {
                rec_state = Rec::idle;
                return false;
            }

            if (mic.capturing()) {
                mic.stop_capture();
            }

            /* The recording peaks far below full scale - speech reads around
               -18dB on the meter - so playing it at the volume setting would
               be inaudible and read as an empty buffer. It goes out at full
               scale instead and the setting is put back at the end, which is a
               replay deliberately louder than the volume control says. */
            rec_volume = spk.volume();
            spk.set_volume(1.0f);

            tone_gen.tone(spk, beep_in_hz, beep_gain, beep_in_s);
            rec_state = Rec::beep_in;
            break;

        case Rec::beep_in:
            if (spk.busy()) {
                break;
            }

            if (!melp_rec.replay(spk)) {
                LOG("rec: replay rejected");
                spk.set_volume(rec_volume);
                rec_state = Rec::idle;
                return false;
            }

            rec_state = Rec::replay;
            break;

        case Rec::replay:
            /* playing() as well as busy(): the codec thread starts the stream,
               so for a tick after replay() returns the speaker is not busy
               yet, which busy() alone would read as a finished replay. */
            if (melp_rec.playing() || spk.busy()) {
                break;
            }

            /* The only view of an underrun there is from out here, and this
               replay is four times longer than anything else this board
               plays. */
            if (spk.last_error() != 0U) {
                LOG("rec: speaker error 0x%08x", (unsigned) spk.last_error());
            }

            if (melp_rec.underruns() != 0U) {
                LOG("rec: %u decode underruns", (unsigned) melp_rec.underruns());
            }

            tone_gen.tone(spk, beep_out_hz, beep_gain, beep_out_s);
            rec_state = Rec::beep_out;
            break;

        case Rec::beep_out:
            if (spk.busy()) {
                break;
            }

            spk.set_volume(rec_volume);
            rec_state = Rec::idle;
            LOG("rec: done");

            /* Returns rather than breaking: the tail of serve_audio() restarts
               the capture on this same tick. */
            return false;
    }

    return true;
}

static void serve_audio() {
    if (serve_record()) {
        return;
    }

    if (sound_requested) {
        sound_requested = false;

        if (mic.capturing()) {
            mic.stop_capture();
        }

        if (!pcm_player.play(spk, pika::audio::meow)) {
            LOG("spk: meow rejected");
        }
        return;
    }

    if (mic_ready && !mic.capturing() && !spk.busy() && !mic.start_capture()) {
        LOG("mic: capture rejected, error 0x%08x", (unsigned) mic.last_error());
        mic_ready = false; /* do not retry ten times a second */
    }
}

/*
 * Formats a 1e-7 degree coordinate as plain decimal degrees. Done in integers
 * because a float carries seven significant digits and this needs ten, and the
 * sign has to be taken off before the split so that -0.5 degrees does not come
 * out as "-0.-5000000".
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
 * The loop ticks ten times a second but only draws on a beat or a UI change,
 * and a flush sends only the pages drawn on since the last one. Waiting a
 * whole second after a keypress would read as a dead button.
 *
 * The panel is flushed only here, so everything on it is drawn from this
 * thread.
 */
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
            lcd.rect<false>(4, 20, 8 * 12, 8);
            lcd.text(4, 20, line);
        }

        /*
         * A level meter that moved once a second would not read as a level
         * meter, so while capturing the block is refreshed every other tick.
         * A flush is up to ~47ms of I2C, but the thread sleeps on the DMA for
         * all of it, so 5Hz costs bus bandwidth rather than CPU.
         */
        const bool mic_due = mic.capturing() && (tick % 2U) == 0U;

        if (beat_due || ui_moved || mic_due) {
            ui.draw();

            /* Only on the record screen, and only where the UI left room:
               drawing over a row it wrote would erase it. */
            if (ui.on_record_screen() && ui.bottom_y() <= mic_text_y) {
                draw_mic();
            }

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

        if (beat_due) {
            beat++;
        }

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

    /* The pin only leaves the plain low output board.c gave it once the PWM is
       running, so the backlight stays dark across the handover. */
    bklt_pwm_cfg.channels[BOARD_LCD_BKLT_PWM_CHANNEL].mode = PWM_OUTPUT_ACTIVE_HIGH;
    pwmStart(&BOARD_LCD_BKLT_PWM, &bklt_pwm_cfg);
    pwmEnableChannel(&BOARD_LCD_BKLT_PWM, BOARD_LCD_BKLT_PWM_CHANNEL,
                     PWM_PERCENTAGE_TO_WIDTH(&BOARD_LCD_BKLT_PWM, 10000U));
    palSetLineMode(LINE_LCD_BKLT, BOARD_LCD_BKLT_PINMODE);

    bool ok = lcd.init();
    LOG("lcd: init %s, i2c error 0x%08x", ok ? "ok" : "failed", (unsigned) lcd.last_error());

    /* The splash is exactly the size of the panel, so it needs no placing. The
       speaker and the microphone come up underneath it and only the remainder
       of splash_ms is slept away, so it costs no time to first beat. */
    systime_t splash_start = chVTGetSystemTimeX();

    if (ok) {
        lcd.clear();
        lcd.bitmap(0, 0, pika::lcd::pika_logo_width, pika::lcd::pika_logo_height, pika::lcd::pika_logo);
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
    /* Before mic.init(): the capture interrupt signals this thread by handle.
       Above NORMALPRIO because it is the only one here with a deadline. */
    melp_rec.start(NORMALPRIO + 1, waMelp, sizeof(waMelp));

    mic.add_consumer(mic_level);
    mic.add_consumer(melp_rec);
    mic_ready = mic.init();
    LOG("mic: init %s", mic_ready ? "ok" : "failed");

    /* What is left of the splash, then the screen the heartbeat writes into.
       Elapsed time rather than a deadline compare, so a wrapped tick counter
       cannot leave this asleep for a very long time. */
    const uint32_t splash_shown_ms = (uint32_t) TIME_I2MS(chVTTimeElapsedSinceX(splash_start));
    if (splash_shown_ms < splash_ms) {
        chThdSleepMilliseconds(splash_ms - splash_shown_ms);
    }

    if (ok) {
        lcd.clear();
        lcd.text(4, 6, BOARD_NAME);
        if (!lcd.flush()) {
            LOG("lcd: flush failed, i2c error 0x%08x", (unsigned) lcd.last_error());
        }
    }

    chThdCreateStatic(waHeartbeat, sizeof(waHeartbeat), NORMALPRIO, heartbeat, nullptr);

    /* Finding the receiver means sweeping the baud rates, which takes seconds
       and has no business holding up the rest of the boot. */
    chThdCreateStatic(waGnss, sizeof(waGnss), NORMALPRIO, gnss_reader, nullptr);
    chThdCreateStatic(waGnssLog, sizeof(waGnssLog), NORMALPRIO, gnss_logger, nullptr);

    /* Applies the initial volume as well as starting the buttons, so it has to
     run before anything can be played. */
    ui.init();

    chThdCreateStatic(waUi, sizeof(waUi), NORMALPRIO, ui_reader, nullptr);

    while (true) { chThdSleepMilliseconds(2000); }
}
