/*
 * Board definitions for Pika Go 0 (STM32H733VGT6).
 *
 * "0" is the device generation: generation 0 is the proof of concept draft,
 * generation 1 will be the first production device. PCB revisions within a
 * generation (rev. A, rev. B, ...) carry bug fixes only, no functional
 * change, so they share this board directory.
 *
 * This is a hand written, deliberately minimal board file: it declares the
 * oscillators and the handful of pins configured in board.c. Fill the real
 * pinout in as the hardware is defined.
 */

#pragma once

/*===========================================================================*/
/* Board identification.                                                     */
/*===========================================================================*/

#define BOARD_PIKA_GO_0
#define BOARD_NAME                  "Pika Go 0"

/*===========================================================================*/
/* MCU and oscillators.                                                      */
/*===========================================================================*/

/*
 * MCU type as defined in the ST header.
 */
#undef STM32H733xx
#define STM32H733xx

/*
 * Board oscillators. The 24MHz HSE oscillator module is powered from the
 * HSE_EN pin, which board.c drives high before stm32_clock_init() runs. The
 * PLL dividers in cfg/mcuconf.h are computed for this frequency, so changing
 * it means revisiting STM32_PLLx_DIVM/DIVN there.
 */
#if !defined(STM32_HSECLK)
#define STM32_HSECLK                24000000U
#endif

/* An oscillator module drives OSC_IN, so the HSE runs in bypass mode. */
#define STM32_HSE_BYPASS

/* No 32.768kHz crystal on this board, see STM32_LSE_ENABLED in mcuconf.h. */
#if !defined(STM32_LSECLK)
#define STM32_LSECLK                0U
#endif

#define STM32_LSEDRV                (3U << 3U)

/*===========================================================================*/
/* Pin assignments.                                                          */
/*===========================================================================*/

/*
 * Board pins, as PAL lines. Their modes are configured in board.c; nothing
 * outside this board directory should mention a port/pad pair.
 *
 * PC15 gates the 24MHz HSE oscillator and therefore has to be driven before
 * stm32_clock_init() runs.
 *
 * PA13/PA14 (SWD) need no entry: they come out of reset as AF0 and board.c
 * resets the GPIO ports on every boot.
 */
#define LINE_HSE_EN                 PAL_LINE(GPIOC, 15U)
#define LINE_LCD_BKLT               PAL_LINE(GPIOE, 9U)
#define LINE_DBG_UART_TX            PAL_LINE(GPIOD, 8U)
#define LINE_DBG_UART_RX            PAL_LINE(GPIOD, 9U)
#define LINE_LCD_COMSCN             PAL_LINE(GPIOD, 7U)
#define LINE_LCD_RST                PAL_LINE(GPIOC, 7U)
#define LINE_LCD_SCL                PAL_LINE(GPIOB, 6U)
#define LINE_LCD_SDA                PAL_LINE(GPIOB, 7U)
#define LINE_SPK_OUT                PAL_LINE(GPIOA, 4U)
#define LINE_SPK_EN                 PAL_LINE(GPIOE, 15U)
#define LINE_MIC_IN                 PAL_LINE(GPIOA, 6U)
#define LINE_MIC_SHDN               PAL_LINE(GPIOA, 7U)
#define LINE_GNSS_TX                PAL_LINE(GPIOD, 5U)
#define LINE_GNSS_RX                PAL_LINE(GPIOD, 6U)
#define LINE_BTN_PWR                PAL_LINE(GPIOE, 10U)
#define LINE_BTN_UP                 PAL_LINE(GPIOD, 10U)
#define LINE_BTN_DOWN               PAL_LINE(GPIOD, 11U)
#define LINE_BTN_RIGHT              PAL_LINE(GPIOD, 14U)
#define LINE_BTN_LEFT               PAL_LINE(GPIOC, 4U)
#define LINE_BTN_PTT                PAL_LINE(GPIOC, 5U)
#define LINE_BTN_LSN                PAL_LINE(GPIOB, 2U)
#define LINE_RADIO_RST              PAL_LINE(GPIOA, 0U)
#define LINE_RADIO_RXEN             PAL_LINE(GPIOA, 1U)
#define LINE_RADIO_TXEN             PAL_LINE(GPIOA, 2U)
#define LINE_RADIO_SPI_SCK          PAL_LINE(GPIOA, 9U)
#define LINE_RADIO_SPI_NSS          PAL_LINE(GPIOA, 15U)
#define LINE_RADIO_SPI_MISO         PAL_LINE(GPIOB, 14U)
#define LINE_RADIO_SPI_MOSI         PAL_LINE(GPIOB, 15U)
#define LINE_RADIO_BUSY             PAL_LINE(GPIOE, 7U)
#define LINE_RADIO_DIO1             PAL_LINE(GPIOE, 8U)
#define LINE_CHG_INT                PAL_LINE(GPIOE, 0U)
#define LINE_CHG_I2C_SCL            PAL_LINE(GPIOD, 12U)
#define LINE_CHG_I2C_SDA            PAL_LINE(GPIOD, 13U)

/* Serial driver behind the debug UART pins above. */
#define BOARD_DBG_SERIAL            SD3

/*
 * NHD-C160100DiZ-FSW-FBW display (160x100, ST75160i controller) on I2C1.
 *
 * The address is the 7 bit one, as ChibiOS expects it. LINE_LCD_COMSCN goes
 * to FPC pin 1, which is not connected on a Rev1C module: driving it high or
 * low changes nothing, confirmed on hardware, so board.c leaves it alone. It
 * becomes COMSCN again on Rev1D.
 */
#define BOARD_LCD_I2C               I2CD1
#define BOARD_LCD_I2C_ADDR          0x3FU

/* Pin mode for SCL/SDA, also used by the driver's bus recovery. */
#define BOARD_LCD_I2C_PINMODE       (PAL_MODE_ALTERNATE(4U) |               \
                                     PAL_STM32_OTYPE_OPENDRAIN |            \
                                     PAL_STM32_OSPEED_MID2)

/*
 * Backlight brightness, as PWM rather than a plain output.
 *
 * LINE_LCD_BKLT is PE9 = TIM1_CH1 on AF1, driving the gate of Q1 (BSS138),
 * which switches the backlight string on J4 to ground. The duty cycle is the
 * brightness. The channel is the driver's 0 based index, so CH1 is 0.
 *
 * board.c leaves the pin a plain output, low; main() starts the PWM, switches
 * the pin to AF1 and hands the UI the same driver and channel.
 */
#define BOARD_LCD_BKLT_PWM          PWMD1
#define BOARD_LCD_BKLT_PWM_CHANNEL  0U
#define BOARD_LCD_BKLT_PINMODE      (PAL_MODE_ALTERNATE(1U) |               \
                                     PAL_STM32_OSPEED_LOWEST)

/*
 * Speaker, driven through an SSM2305 class D amplifier.
 *
 * LINE_SPK_OUT is DAC1_OUT1, so the DAC drives the pin directly and it stays
 * analog. LINE_SPK_EN is the amplifier's /SD shutdown input, which is active
 * low: the pin comes up low and the amplifier stays muted until the driver
 * asks for sound.
 */
#define BOARD_SPK_DAC               DACD1

/*
 * How long the output has to sit at its idle level before /SD may go high.
 * The amplifier input is a 100nF series capacitor against 100k, so tau is
 * 10ms and five of those gets within 1%. Un-muting before then couples the
 * charging ramp into the amplifier, which is an audible pop.
 */
#define BOARD_SPK_SETTLE_MS         50U

/*
 * The sample clock: one timer's TRGO paces both converters at 32kHz, the DAC
 * on the way out and the ADC on the way in.
 *
 * It is one timer and not two because the speaker and the microphone never run
 * at the same time on this device - it either talks or listens. pika/audio's
 * SampleClock wraps this and enforces that: whichever converter claims the
 * clock holds it until it stops, and the other one is refused.
 */
#define BOARD_SAMPLE_TIMER          GPTD6

/*
 * Microphone: an analog preamp into ADC1.
 *
 * LINE_MIC_IN is ADC1_INP3, so the pin stays analog and the converter reads it
 * directly. LINE_MIC_SHDN is the preamp's shutdown input and is active *high* -
 * the pin is driven low to make the microphone run, which is the opposite of
 * LINE_SPK_EN above. It comes up high so the preamp stays off until the driver
 * asks to listen.
 */
#define BOARD_MIC_ADC               ADCD1
#define BOARD_MIC_ADC_CHANNEL       ADC_CHANNEL_IN3

/*
 * How long to wait after the preamp wakes up before its samples mean anything.
 * The output has to charge its coupling capacitor up to the bias point, and
 * converting through that ramp records it as a thump.
 */
#define BOARD_MIC_SETTLE_MS         20U

/*
 * u-blox CAM-M8Q-0 GNSS receiver on USART2.
 *
 * The line names are from the MCU's point of view, like the debug UART ones:
 * LINE_GNSS_TX is what the MCU drives, so it goes to the receiver's RX pad,
 * and LINE_GNSS_RX carries the receiver's output.
 *
 * The module powers up at 9600 8N1 with both NMEA and UBX enabled on its
 * UART1. The driver talks UBX only and switches the link to 115200; see
 * pika/ublox.h for why it probes the fast rate first.
 *
 * TIMEPULSE, EXTINT, SAFEBOOT and RESET_N are not wired on this generation.
 */
#define BOARD_GNSS_SERIAL           SD2
#define BOARD_GNSS_BAUD_BOOT        9600U
#define BOARD_GNSS_BAUD_RUN         115200U

/*
 * Semtech SX1262 sub-GHz radio on SPI2, AF5 on SCK/MISO/MOSI.
 *
 * NSS is not on AF5: ChibiOS drives the chip select itself as a plain GPIO
 * (SPI_SELECT_MODE_PAD, the default), which is also what the SX1262 wants -
 * it ends a command on the rising edge of NSS rather than on a frame count.
 *
 * LINE_RADIO_RST is active low and the pin comes up low, so the chip stays in
 * reset until the driver pulses it. RXEN/TXEN switch the front end and are
 * active high, both low means neither path is enabled. BUSY is polled before
 * every command; DIO1 is the done interrupt and uses EXTI channel 8, which no
 * button claims (see the note above for why that matters).
 */
#define BOARD_RADIO_SPI             SPID2
#define BOARD_RADIO_SPI_PINMODE     (PAL_MODE_ALTERNATE(5U) |                \
                                     PAL_STM32_OSPEED_HIGHEST)

/*
 * TI BQ25895 battery charger on I2C4, AF4 on SCL/SDA.
 *
 * The address is the 7 bit one. SCL, SDA and LINE_CHG_INT have 10k pull-ups
 * to 3.3V next to the charger, so the pins use no internal ones. INT is
 * active low and open drain: the charger pulses it low for 256us on a fault
 * or a status change. It uses EXTI channel 0, which nothing else claims.
 *
 * I2C4 sits in the D3 domain and is served by the BDMA, which only reaches
 * SRAM4 - the D2 SRAM buffers the LCD driver uses are out of its range.
 */
#define BOARD_CHG_I2C               I2CD4
#define BOARD_CHG_I2C_ADDR          0x6AU

/* Pin mode for SCL/SDA. */
#define BOARD_CHG_I2C_PINMODE       (PAL_MODE_ALTERNATE(4U) |               \
                                     PAL_STM32_OTYPE_OPENDRAIN |            \
                                     PAL_STM32_OSPEED_MID2)

/*
 * Charge current and voltage the driver programs at init.
 *
 * These are the BQ25895's own power-on values, i.e. exactly what the board
 * charged with before the driver existed - not a choice made for a cell. The
 * battery on J1 is not specified anywhere in the hardware repo; set these from
 * its datasheet (typically 0.5C to 1C, and 4.2V for a plain LiPo). The driver
 * disables the chip's I2C watchdog, so whatever is set here stays in force
 * even if the firmware hangs.
 *
 * What else bounds charging on this board, from the schematic:
 *   - CE is strapped low, so the chip charges with or without firmware, and
 *     OTG is strapped low, so boost mode is not available.
 *   - R4 = 261R on ILIM caps the input at 355/261 = 1.36A.
 *   - D+/D- are not connected, so input detection cannot identify the
 *     adapter and settles on "unknown", which limits the input to 500mA.
 *   - TS has the datasheet's 5.23k/30.1k network for a 10k NTC on J1 pin 2,
 *     so JEITA temperature limits apply.
 *   - ~QON is wired to BTN_PWR: a long press exits ship mode or resets the
 *     charger, independent of the MCU.
 */
#define BOARD_CHG_ICHG_MA           2048U
#define BOARD_CHG_VREG_MV           4208U

/*===========================================================================*/
/* External declarations.                                                    */
/*===========================================================================*/

#if !defined(_FROM_ASM_)
#ifdef __cplusplus
extern "C" {
#endif
  void boardInit(void);
#ifdef __cplusplus
}
#endif
#endif /* _FROM_ASM_ */

