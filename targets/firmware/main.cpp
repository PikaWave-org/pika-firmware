#include "ch.h"
#include "hal.h"

#include "chprintf.h"

#include <algorithm>
#include <cmath>

#include <pika/App.h>
#include <pika/MAC.h>
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
#include <pika/power/BQ25895.h>
#include <pika/radio/SX1262.h>
#include <pika/ublox.h>

/* The panel, wired up as the board header describes it. */
static const pika::lcd::ST75160::Config display_cfg = {&BOARD_LCD_I2C, BOARD_LCD_I2C_ADDR, LINE_LCD_RST,
                                                       LINE_LCD_SCL,   LINE_LCD_SDA,       BOARD_LCD_I2C_PINMODE};

static pika::lcd::ST75160 display{display_cfg};

/* The battery charger, wired up and charging as the board header says. */
static const pika::power::BQ25895::Config charger_cfg = {&BOARD_CHG_I2C, BOARD_CHG_I2C_ADDR, LINE_CHG_INT,
                                                         BOARD_CHG_ICHG_MA, BOARD_CHG_VREG_MV};

static pika::power::BQ25895 charger{charger_cfg};

static constexpr uint32_t bklt_pwm_hz = 1000000U;
static constexpr pwmcnt_t bklt_period = 1000U;

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

/* BTN_PWR is the polled one: it shares EXTI channel 10 with BTN_UP and the
   channel serves one port at a time, see board.h. */
static const pika::input::Buttons::Config btn_cfg = {.lines = {{LINE_BTN_PWR, true},
                                                               {LINE_BTN_UP, false},
                                                               {LINE_BTN_DOWN, false},
                                                               {LINE_BTN_RIGHT, false},
                                                               {LINE_BTN_LEFT, false},
                                                               {LINE_BTN_PTT, false},
                                                               {LINE_BTN_LSN, false}},
                                                     .event_offset = 0};

static const pika::gnss::Ublox::Config gnss_cfg = {&BOARD_GNSS_SERIAL, BOARD_GNSS_BAUD_RUN, 1000U, 1U};
static pika::gnss::Ublox gnss{gnss_cfg};

/*
 * The SX1262 radio on SPI2, see the board header for the pins.
 *
 * cfg1/cfg2 are the STM32H7 SPI registers the low level driver takes as they
 * are, apart from MASTER and SSOE which it sets itself:
 *   MBR = 1 divides the SPI2 kernel clock - PLL1_Q, 52MHz per mcuconf.h - by
 *   4, giving 13MHz, under the SX1262's 16MHz ceiling.
 *   DSIZE = 7 is 8 bit frames, and cfg2 = 0 leaves CPOL = CPHA = 0 (SPI mode
 *   0, what the radio expects) and MSB first.
 *
 * The chip select is a GPIO driven by the driver, so it goes in as a port and
 * pad rather than an alternate function pin.
 */
static const SPIConfig radio_spi_cfg = {
        .circular = false,
        .slave = false,
        .data_cb = nullptr,
        .error_cb = nullptr,
        .ssport = PAL_PORT(LINE_RADIO_SPI_NSS),
        .sspad = PAL_PAD(LINE_RADIO_SPI_NSS),
        .cfg1 = SPI_CFG1_MBR_0 | (7U << SPI_CFG1_DSIZE_Pos),
        .cfg2 = 0U,
};

static pika::radio::SX1262 radio{BOARD_RADIO_SPI, LINE_RADIO_RST,  LINE_RADIO_BUSY,
                                 LINE_RADIO_DIO1, LINE_RADIO_TXEN, LINE_RADIO_RXEN};

static constexpr size_t radio_payload_len = 128U;

static pika::MAC mac{&radio};

static const pika::App::Config app_cfg = {.display = &display,
                                          .speaker = &spk,
                                          .mic = &mic,
                                          .buttons = btn_cfg,
                                          .mac = &mac,
                                          .charger = &charger,
                                          .backlight_pwm = &BOARD_LCD_BKLT_PWM,
                                          .backlight_ch = BOARD_LCD_BKLT_PWM_CHANNEL};

static pika::App app{app_cfg};

int main() {
    halInit();
    chSysInit();

    pika::log_init();
    LOG("\r\n" BOARD_NAME " starting");

    bklt_pwm_cfg.channels[BOARD_LCD_BKLT_PWM_CHANNEL].mode = PWM_OUTPUT_ACTIVE_HIGH;
    pwmStart(&BOARD_LCD_BKLT_PWM, &bklt_pwm_cfg);
    palSetLineMode(LINE_LCD_BKLT, BOARD_LCD_BKLT_PINMODE);

    bool display_ok = display.init();
    LOG("lcd: init %s, i2c error 0x%08x", display_ok ? "ok" : "failed", (unsigned) display.last_error());

    /* Initialises the chip itself, then updates on every ~INT pulse. */
    charger.start(NORMALPRIO);

    bool spk_ok = spk.init();
    LOG("spk: init %s", spk_ok ? "ok" : "failed");

    bool mic_ok = mic.init();
    LOG("mic: init %s", mic_ok ? "ok" : "failed");

    spiStart(&BOARD_RADIO_SPI, &radio_spi_cfg);
    LOG("radio: init");
    radio.set_mod_params(9, 5, 5);
    radio.set_power(10.0f);
    radio.init(radio_payload_len);
    LOG("radio: init done");

    mac.start(HIGHPRIO);

    app.init();
    app.start(NORMALPRIO);

    while (true) {
        const pika::BatteryStatus &chg = charger.status();
        LOG("chg: input %u charge %u vbat %umV ichg %umA, i2c error 0x%08x", (unsigned) chg.input_connected,
            (unsigned) chg.charging, (unsigned) chg.voltage, (unsigned) chg.charge_current,
            (unsigned) charger.last_error());

        chThdSleepMilliseconds(1000);
    }
}
