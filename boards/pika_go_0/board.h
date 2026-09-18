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
 * Front panel buttons.
 *
 * All seven short their line to ground when pressed and rely on the MCU's
 * internal pull-up, so a pressed button reads low.
 *
 * Six of them are interrupt driven, but BTN_PWR cannot be: an EXTI channel is
 * selected by pad number and can be mapped to only one port at a time, and
 * PE10 and PD10 both want channel 10. ChibiOS catches the second one with a
 * "channel already in use" assertion. PD10 (BTN_UP) keeps the interrupt and
 * PE10 is polled instead, which costs it up to BOARD_BTN_POLL_MS of latency.
 * Moving BTN_PWR to a pad whose number no other button uses would let the
 * driver drop its polling path entirely.
 *
 * The remaining six sit on channels 10, 11, 14, 4, 5 and 2, all distinct.
 */
#define BOARD_BTN_DEBOUNCE_MS       20U
#define BOARD_BTN_POLL_MS           10U

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

