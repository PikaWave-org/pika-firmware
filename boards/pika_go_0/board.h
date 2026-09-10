/*
 * Board definitions for the Pika GO rev.0 board (STM32H733VGT6).
 *
 * This is a hand written, deliberately minimal board file: every pin defaults
 * to analog input (the lowest power, safest state for a pin whose function is
 * not decided yet) and only the pins listed under "Pin assignments" below get
 * a different configuration. Fill the real pinout in as the hardware is
 * defined; the per-port VAL_GPIOx_* macros at the bottom are the only place
 * that needs to change.
 */

#ifndef BOARD_H
#define BOARD_H

/*===========================================================================*/
/* Board identification.                                                     */
/*===========================================================================*/

#define BOARD_PIKA_GO_0
#define BOARD_NAME                  "Pika GO rev.0"

/*===========================================================================*/
/* MCU and oscillators.                                                      */
/*===========================================================================*/

/*
 * MCU type as defined in the ST header.
 */
#undef STM32H733xx
#define STM32H733xx

/*
 * Board oscillators. STM32_HSECLK is the external crystal fitted on the
 * board; the PLL dividers in cfg/mcuconf.h are computed for this value, so
 * changing it means revisiting STM32_PLLx_DIVM/DIVN there.
 */
#if !defined(STM32_HSECLK)
#define STM32_HSECLK                25000000U
#endif

/* No 32.768kHz crystal on this board, see STM32_LSE_ENABLED in mcuconf.h. */
#if !defined(STM32_LSECLK)
#define STM32_LSECLK                0U
#endif

#define STM32_LSEDRV                (3U << 3U)

/*===========================================================================*/
/* Pin assignments.                                                          */
/*===========================================================================*/

/* Debug port, kept as alternate function 0 so SWD stays alive. */
#define GPIOA_SWDIO                 13U
#define GPIOA_SWCLK                 14U

/* Heartbeat LED. TODO: point at the real LED pin. */
#define GPIOB_LED                   0U

/*===========================================================================*/
/* GPIO register values.                                                     */
/*===========================================================================*/

#define PIN_MODE_MASK(n)            (3U << (2U * (n)))
#define PIN_MODE_INPUT(n)           (0U << (2U * (n)))
#define PIN_MODE_OUTPUT(n)          (1U << (2U * (n)))
#define PIN_MODE_ALTERNATE(n)       (2U << (2U * (n)))
#define PIN_MODE_ANALOG(n)          (3U << (2U * (n)))
#define PIN_OSPEED_MASK(n)          (3U << (2U * (n)))
#define PIN_OSPEED_HIGH(n)          (2U << (2U * (n)))
#define PIN_PUPDR_MASK(n)           (3U << (2U * (n)))
#define PIN_PUPDR_PULLUP(n)         (1U << (2U * (n)))
#define PIN_ODR_HIGH(n)             (1U << (n))
#define PIN_AFIO_AF(n, v)           ((v) << (4U * ((n) % 8U)))

/* Defaults applied to every port: all pins analog, push-pull, very low speed,
   no pull, output latches low, alternate function 0. */
#define VAL_GPIO_MODER_DEFAULT      0xFFFFFFFFU
#define VAL_GPIO_OTYPER_DEFAULT     0x00000000U
#define VAL_GPIO_OSPEEDR_DEFAULT    0x00000000U
#define VAL_GPIO_PUPDR_DEFAULT      0x00000000U
#define VAL_GPIO_ODR_DEFAULT        0x00000000U
#define VAL_GPIO_AFRL_DEFAULT       0x00000000U
#define VAL_GPIO_AFRH_DEFAULT       0x00000000U

/* Port A: PA13/PA14 are SWDIO/SWCLK. */
#define VAL_GPIOA_MODER             ((VAL_GPIO_MODER_DEFAULT &              \
                                      ~(PIN_MODE_MASK(GPIOA_SWDIO) |        \
                                        PIN_MODE_MASK(GPIOA_SWCLK))) |      \
                                     PIN_MODE_ALTERNATE(GPIOA_SWDIO) |      \
                                     PIN_MODE_ALTERNATE(GPIOA_SWCLK))
#define VAL_GPIOA_OTYPER            VAL_GPIO_OTYPER_DEFAULT
#define VAL_GPIOA_OSPEEDR           (VAL_GPIO_OSPEEDR_DEFAULT |             \
                                     PIN_OSPEED_HIGH(GPIOA_SWDIO) |         \
                                     PIN_OSPEED_HIGH(GPIOA_SWCLK))
#define VAL_GPIOA_PUPDR             (VAL_GPIO_PUPDR_DEFAULT |               \
                                     PIN_PUPDR_PULLUP(GPIOA_SWDIO))
#define VAL_GPIOA_ODR               VAL_GPIO_ODR_DEFAULT
#define VAL_GPIOA_AFRL              VAL_GPIO_AFRL_DEFAULT
#define VAL_GPIOA_AFRH              VAL_GPIO_AFRH_DEFAULT

/* Port B: heartbeat LED. */
#define VAL_GPIOB_MODER             ((VAL_GPIO_MODER_DEFAULT &              \
                                      ~PIN_MODE_MASK(GPIOB_LED)) |          \
                                     PIN_MODE_OUTPUT(GPIOB_LED))
#define VAL_GPIOB_OTYPER            VAL_GPIO_OTYPER_DEFAULT
#define VAL_GPIOB_OSPEEDR           VAL_GPIO_OSPEEDR_DEFAULT
#define VAL_GPIOB_PUPDR             VAL_GPIO_PUPDR_DEFAULT
#define VAL_GPIOB_ODR               VAL_GPIO_ODR_DEFAULT
#define VAL_GPIOB_AFRL              VAL_GPIO_AFRL_DEFAULT
#define VAL_GPIOB_AFRH              VAL_GPIO_AFRH_DEFAULT

/* Ports C..K: nothing assigned yet. */
#define VAL_GPIOC_MODER             VAL_GPIO_MODER_DEFAULT
#define VAL_GPIOC_OTYPER            VAL_GPIO_OTYPER_DEFAULT
#define VAL_GPIOC_OSPEEDR           VAL_GPIO_OSPEEDR_DEFAULT
#define VAL_GPIOC_PUPDR             VAL_GPIO_PUPDR_DEFAULT
#define VAL_GPIOC_ODR               VAL_GPIO_ODR_DEFAULT
#define VAL_GPIOC_AFRL              VAL_GPIO_AFRL_DEFAULT
#define VAL_GPIOC_AFRH              VAL_GPIO_AFRH_DEFAULT

#define VAL_GPIOD_MODER             VAL_GPIO_MODER_DEFAULT
#define VAL_GPIOD_OTYPER            VAL_GPIO_OTYPER_DEFAULT
#define VAL_GPIOD_OSPEEDR           VAL_GPIO_OSPEEDR_DEFAULT
#define VAL_GPIOD_PUPDR             VAL_GPIO_PUPDR_DEFAULT
#define VAL_GPIOD_ODR               VAL_GPIO_ODR_DEFAULT
#define VAL_GPIOD_AFRL              VAL_GPIO_AFRL_DEFAULT
#define VAL_GPIOD_AFRH              VAL_GPIO_AFRH_DEFAULT

#define VAL_GPIOE_MODER             VAL_GPIO_MODER_DEFAULT
#define VAL_GPIOE_OTYPER            VAL_GPIO_OTYPER_DEFAULT
#define VAL_GPIOE_OSPEEDR           VAL_GPIO_OSPEEDR_DEFAULT
#define VAL_GPIOE_PUPDR             VAL_GPIO_PUPDR_DEFAULT
#define VAL_GPIOE_ODR               VAL_GPIO_ODR_DEFAULT
#define VAL_GPIOE_AFRL              VAL_GPIO_AFRL_DEFAULT
#define VAL_GPIOE_AFRH              VAL_GPIO_AFRH_DEFAULT

#define VAL_GPIOF_MODER             VAL_GPIO_MODER_DEFAULT
#define VAL_GPIOF_OTYPER            VAL_GPIO_OTYPER_DEFAULT
#define VAL_GPIOF_OSPEEDR           VAL_GPIO_OSPEEDR_DEFAULT
#define VAL_GPIOF_PUPDR             VAL_GPIO_PUPDR_DEFAULT
#define VAL_GPIOF_ODR               VAL_GPIO_ODR_DEFAULT
#define VAL_GPIOF_AFRL              VAL_GPIO_AFRL_DEFAULT
#define VAL_GPIOF_AFRH              VAL_GPIO_AFRH_DEFAULT

#define VAL_GPIOG_MODER             VAL_GPIO_MODER_DEFAULT
#define VAL_GPIOG_OTYPER            VAL_GPIO_OTYPER_DEFAULT
#define VAL_GPIOG_OSPEEDR           VAL_GPIO_OSPEEDR_DEFAULT
#define VAL_GPIOG_PUPDR             VAL_GPIO_PUPDR_DEFAULT
#define VAL_GPIOG_ODR               VAL_GPIO_ODR_DEFAULT
#define VAL_GPIOG_AFRL              VAL_GPIO_AFRL_DEFAULT
#define VAL_GPIOG_AFRH              VAL_GPIO_AFRH_DEFAULT

#define VAL_GPIOH_MODER             VAL_GPIO_MODER_DEFAULT
#define VAL_GPIOH_OTYPER            VAL_GPIO_OTYPER_DEFAULT
#define VAL_GPIOH_OSPEEDR           VAL_GPIO_OSPEEDR_DEFAULT
#define VAL_GPIOH_PUPDR             VAL_GPIO_PUPDR_DEFAULT
#define VAL_GPIOH_ODR               VAL_GPIO_ODR_DEFAULT
#define VAL_GPIOH_AFRL              VAL_GPIO_AFRL_DEFAULT
#define VAL_GPIOH_AFRH              VAL_GPIO_AFRH_DEFAULT

#define VAL_GPIOI_MODER             VAL_GPIO_MODER_DEFAULT
#define VAL_GPIOI_OTYPER            VAL_GPIO_OTYPER_DEFAULT
#define VAL_GPIOI_OSPEEDR           VAL_GPIO_OSPEEDR_DEFAULT
#define VAL_GPIOI_PUPDR             VAL_GPIO_PUPDR_DEFAULT
#define VAL_GPIOI_ODR               VAL_GPIO_ODR_DEFAULT
#define VAL_GPIOI_AFRL              VAL_GPIO_AFRL_DEFAULT
#define VAL_GPIOI_AFRH              VAL_GPIO_AFRH_DEFAULT

#define VAL_GPIOJ_MODER             VAL_GPIO_MODER_DEFAULT
#define VAL_GPIOJ_OTYPER            VAL_GPIO_OTYPER_DEFAULT
#define VAL_GPIOJ_OSPEEDR           VAL_GPIO_OSPEEDR_DEFAULT
#define VAL_GPIOJ_PUPDR             VAL_GPIO_PUPDR_DEFAULT
#define VAL_GPIOJ_ODR               VAL_GPIO_ODR_DEFAULT
#define VAL_GPIOJ_AFRL              VAL_GPIO_AFRL_DEFAULT
#define VAL_GPIOJ_AFRH              VAL_GPIO_AFRH_DEFAULT

#define VAL_GPIOK_MODER             VAL_GPIO_MODER_DEFAULT
#define VAL_GPIOK_OTYPER            VAL_GPIO_OTYPER_DEFAULT
#define VAL_GPIOK_OSPEEDR           VAL_GPIO_OSPEEDR_DEFAULT
#define VAL_GPIOK_PUPDR             VAL_GPIO_PUPDR_DEFAULT
#define VAL_GPIOK_ODR               VAL_GPIO_ODR_DEFAULT
#define VAL_GPIOK_AFRL              VAL_GPIO_AFRL_DEFAULT
#define VAL_GPIOK_AFRH              VAL_GPIO_AFRH_DEFAULT

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
