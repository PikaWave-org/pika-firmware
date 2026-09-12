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

#ifndef BOARD_H
#define BOARD_H

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

#endif /* BOARD_H */
