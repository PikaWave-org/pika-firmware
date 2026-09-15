#include "ch.h"
#include "hal.h"

#include "chprintf.h"

#include <pika/audio/DACSpeaker.h>
#include <pika/audio/PCMPlayer.h>
#include <pika/audio/sounds/meow.h>
#include <pika/audio/tone_generator.h>
#include <pika/log.h>
#include <pika/st75160.h>
#include <pika/ublox.h>

/* The panel, wired up as the board header describes it. */
static const pika::lcd::St75160::Config lcd_cfg = {&BOARD_LCD_I2C,       BOARD_LCD_I2C_ADDR, LINE_LCD_RST,
                                                   LINE_LCD_BKLT,        LINE_LCD_SCL,       LINE_LCD_SDA,
                                                   BOARD_LCD_I2C_PINMODE};

static pika::lcd::St75160 lcd{lcd_cfg};

/* The speaker, likewise. */
static const pika::audio::DACSpeaker::Config spk_cfg = {&BOARD_SPK_DAC, &BOARD_SPK_TIMER, LINE_SPK_EN,
                                                        BOARD_SPK_SETTLE_MS};

static pika::audio::DACSpeaker spk{spk_cfg};
static pika::audio::ToneGenerator tone_gen;
static pika::audio::PCMPlayer pcm_player;

/* The GNSS receiver, at one navigation solution per second on its UART1. */
static const pika::gnss::Ublox::Config gnss_cfg = {&BOARD_GNSS_SERIAL, BOARD_GNSS_BAUD_RUN, 1000U, 1U};

static pika::gnss::Ublox gnss{gnss_cfg};

/* Last measured frame time, in milliseconds. */
uint32_t flush_ms;

/*
 * Top of the GNSS status block on the panel, four lines 10 pixels apart. The
 * title sits at y = 6 and the beat counter at 20, and the last line lands at
 * 60..67, just above the checkerboard the bring-up image draws at 68.
 */
static constexpr int gnss_y = 30;

/* Widest field is "lat -123.1234567", 16 glyphs. */
static constexpr int status_glyphs = 16;

static_assert(4 + 8 * status_glyphs <= pika::lcd::St75160::width, "status text runs past the right edge of the panel");
static_assert(gnss_y + 30 + 8 <= 68, "status text overlaps the bring-up checkerboard at y = 68");

/*
 * Bring-up image: a border, a checkerboard block, a diagonal and a text
 * line. Between them these show orientation, mirroring, the pixel grid and
 * any dead rows or columns.
 */
static void draw_test_image() {

    lcd.clear();
    lcd.text(4, 6, BOARD_NAME);
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

/*
 * One line of status text at the left edge, cleared before it is redrawn so
 * a shorter value cannot leave the tail of the previous one behind. 16 glyphs
 * is as wide as the longest field gets ("lat -123.1234567") and still clears
 * the border at x = 159.
 */
static void status_line(int y, const char *s) {

    lcd.rect(4, y, 8 * status_glyphs, 8, false);
    lcd.text(4, y, s);
}

/*
 * The navigation solution on the panel: fix, position, altitude. Deliberately
 * the short version - the debug UART carries the accuracies and the counters,
 * and there are only 20 glyphs to a line here.
 */
static void draw_gnss() {

    pika::gnss::msg_rx_nav_pvt_s pvt;

    if (!gnss.nav_pvt(pvt)) {
        status_line(gnss_y, "fix --");
        status_line(gnss_y + 10, "lat --");
        status_line(gnss_y + 20, "lon --");
        status_line(gnss_y + 30, "alt --");
        return;
    }

    char line[24];
    char deg[16];

    chsnprintf(line, sizeof line, "iTOW %u", (unsigned) (pvt.iTOW / 1000));
    status_line(gnss_y, line);

    chsnprintf(line, sizeof line, "fix %u sv %u", (unsigned) pvt.fixType, (unsigned) pvt.numSV);
    status_line(gnss_y + 10, line);

    format_deg(deg, sizeof deg, pvt.lat);
    chsnprintf(line, sizeof line, "lat %s", deg);
    status_line(gnss_y + 20, line);

    format_deg(deg, sizeof deg, pvt.lon);
    chsnprintf(line, sizeof line, "lon %s", deg);
    status_line(gnss_y + 30, line);

    chsnprintf(line, sizeof line, "alt %dm", (int) (pvt.hMSL / 1000));
    status_line(gnss_y + 40, line);
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

    if (!gnss.nav_pvt(pvt)) {
        LOG("gnss: no solution yet, frames %u errors %u", (unsigned) gnss.frames(), (unsigned) gnss.errors());
        return;
    }

    /* A "!" marks a solution the receiver itself does not trust. */
    bool fix_ok = (pvt.flags & 0x01U) != 0U;

    char lat[16], lon[16];
    format_deg(lat, sizeof lat, pvt.lat);
    format_deg(lon, sizeof lon, pvt.lon);

    LOG("gnss: iTOW=%u fix=%u%s sv=%u lat=%s lon=%s alt=%dm hacc=%um "
        "pdop=%u.%02u "
        "frames=%u errors=%u",
        (unsigned) (pvt.iTOW / 1000), (unsigned) pvt.fixType, fix_ok ? "" : "!", (unsigned) pvt.numSV, lat, lon,
        (int) (pvt.hMSL / 1000), (unsigned) (pvt.hAcc / 1000U), (unsigned) (pvt.pDOP / 100U),
        (unsigned) (pvt.pDOP % 100U), (unsigned) gnss.frames(), (unsigned) gnss.errors());

    /* Date, time and the UTC offset all have to be resolved before the
     timestamp is worth printing.*/
    if ((pvt.valid & 0x07U) == 0x07U) {
        LOG("gnss: utc %04u-%02u-%02u %02u:%02u:%02u", (unsigned) pvt.year, (unsigned) pvt.month, (unsigned) pvt.day,
            (unsigned) pvt.hour, (unsigned) pvt.min, (unsigned) pvt.sec);
    }
}

/*
 * Heartbeat: a line on the debug UART and a refreshed counter on the LCD,
 * once per second, and the GNSS status to both.
 *
 * The panel is only flushed here, so everything drawn on it has to be drawn
 * from this thread: one I2C transaction per second for the whole frame.
 */
static THD_WORKING_AREA(waHeartbeat, 2048);

static THD_FUNCTION(heartbeat, arg) {

    (void) arg;
    chRegSetThreadName("heartbeat");

    uint32_t beat = 0;

    while (true) {
        LOG("heartbeat %u", (unsigned) beat);

        char line[24];
        chsnprintf(line, sizeof line, "beat %u", beat);
        lcd.rect(4, 20, 8 * 12, 8, false);
        lcd.text(4, 20, line);

        draw_gnss();
        log_gnss();

        systime_t start = chVTGetSystemTimeX();
        bool ok = lcd.flush();
        sysinterval_t took = chVTTimeElapsedSinceX(start);
        flush_ms = (uint32_t) TIME_I2MS(took);

        if (!ok) {
            LOG("lcd: flush failed, i2c error 0x%08x", (unsigned) lcd.last_error());
        } else if (beat == 0) {
            LOG("lcd: flush took %ums", (unsigned) flush_ms);
        }

        beat++;
        chThdSleepMilliseconds(1000);
    }
}

/* The driver owns this thread: run() configures the receiver, then reads. */
static THD_WORKING_AREA(waGnss, 2048);

static THD_FUNCTION(gnss_reader, arg) {

    (void) arg;
    chRegSetThreadName("gnss");

    gnss.run();
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
        draw_test_image();
        if (!lcd.flush()) {
            LOG("lcd: flush failed, i2c error 0x%08x", (unsigned) lcd.last_error());
        }
    }

    /* Speaker. verify() runs with the amplifier muted, so it makes no sound:
     it is there to catch a DMA path that moves nothing, which otherwise
     looks exactly like a working one until you put an ear to the speaker.*/
    bool spk_ok = spk.init();
    LOG("spk: init %s", spk_ok ? "ok" : "failed");

    /* The 0.2 the greeting used to be scaled by, now that level is the
     speaker's business rather than each source's. */
    spk.set_volume(0.2f);

    /* The GNSS driver configures the receiver from its own thread: finding it
     means sweeping the baud rates, which takes seconds and has no business
     holding up the rest of the boot.*/
    chThdCreateStatic(waGnss, sizeof(waGnss), NORMALPRIO, gnss_reader, nullptr);

    chThdCreateStatic(waHeartbeat, sizeof(waHeartbeat), NORMALPRIO, heartbeat, nullptr);

    using namespace pika::audio;
    if (!pcm_player.play(spk, meow)) {
        LOG("spk: meow rejected");
    }

    while (true) {
        chThdSleepMilliseconds(2000);
    }
}
