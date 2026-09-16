/*
 * Board initialization for the Pika Go 0 board (STM32H733VGT6).
 *
 * Pin names come from board.h; this file configures their modes at startup,
 * before the clock tree is initialized. Pins that are not named here keep
 * their reset state (analog, no pull).
 */

#include "hal.h"

/*===========================================================================*/
/* Driver local definitions.                                                 */
/*===========================================================================*/

/*===========================================================================*/
/* Driver exported variables.                                                */
/*===========================================================================*/

/*===========================================================================*/
/* Driver local functions.                                                   */
/*===========================================================================*/

/*
 * D2 SRAM (0x30000000) has its own clock gates and comes out of reset with
 * them off, so the memory silently ignores accesses until they are enabled.
 * The I2C DMA buffers live there, so this has to happen before any of it is
 * touched. The .nocache section is NOLOAD, so startup never writes to it.
 */
static void stm32_d2_sram_init(void) { rccEnableAHB2(RCC_AHB2ENR_SRAM1EN | RCC_AHB2ENR_SRAM2EN, false); }

static void stm32_gpio_init(void) {

    /* Enabling GPIO-related clocks, the mask comes from the
     registry header file.*/
    __rccResetAHB4(STM32_GPIO_EN_MASK);
    rccEnableAHB4(STM32_GPIO_EN_MASK, true);

    palSetLineMode(LINE_HSE_EN, PAL_MODE_OUTPUT_PUSHPULL);
    palSetLineMode(LINE_LCD_BKLT, PAL_MODE_OUTPUT_PUSHPULL);
    palSetLineMode(LINE_DBG_UART_TX, PAL_MODE_ALTERNATE(7U) | PAL_STM32_OSPEED_MID2);
    palSetLineMode(LINE_DBG_UART_RX, PAL_MODE_ALTERNATE(7U));

    /* Display: held in reset (RST is active low) until the driver initializes
     it, COM scan direction low, I2C1 on AF4.*/
    palSetLineMode(LINE_LCD_RST, PAL_MODE_OUTPUT_PUSHPULL);
    /* The board carries 5.1k pull-ups on SCL and SDA, so no internal ones.*/
    palSetLineMode(LINE_LCD_SCL, BOARD_LCD_I2C_PINMODE);
    palSetLineMode(LINE_LCD_SDA, BOARD_LCD_I2C_PINMODE);

    /* Speaker: SPK_OUT is DAC1_OUT1 and stays analog, SPK_EN is the
     amplifier's active low shutdown. ODR resets to 0, but clear it
     explicitly before the pin becomes an output so the intent survives a
     reordering of this function: a high glitch on /SD un-mutes the
     amplifier while its input is at an undefined level.*/
    palClearLine(LINE_SPK_EN);
    palSetLineMode(LINE_SPK_EN, PAL_MODE_OUTPUT_PUSHPULL);
    palSetLineMode(LINE_SPK_OUT, PAL_MODE_INPUT_ANALOG);

    palSetLine(LINE_MIC_EN);
    palSetLineMode(LINE_MIC_EN, PAL_MODE_OUTPUT_PUSHPULL);

    /* GNSS receiver on USART2, AF7. The pull-up on RX keeps the line idle high
     while the module is still booting, so the USART sees no framing errors.*/
    palSetLineMode(LINE_GNSS_TX, PAL_MODE_ALTERNATE(7U) | PAL_STM32_OSPEED_MID2);
    palSetLineMode(LINE_GNSS_RX, PAL_MODE_ALTERNATE(7U) | PAL_STM32_PUPDR_PULLUP);

    /* Front panel buttons. They short to ground, so the internal pull-up is
     what holds the line high while the button is up; see board.h for why
     BTN_PWR ends up polled rather than interrupt driven.*/
    palSetLineMode(LINE_BTN_PWR, PAL_MODE_INPUT_PULLUP);
    palSetLineMode(LINE_BTN_UP, PAL_MODE_INPUT_PULLUP);
    palSetLineMode(LINE_BTN_DOWN, PAL_MODE_INPUT_PULLUP);
    palSetLineMode(LINE_BTN_RIGHT, PAL_MODE_INPUT_PULLUP);
    palSetLineMode(LINE_BTN_LEFT, PAL_MODE_INPUT_PULLUP);
    palSetLineMode(LINE_BTN_PTT, PAL_MODE_INPUT_PULLUP);
    palSetLineMode(LINE_BTN_LSN, PAL_MODE_INPUT_PULLUP);

    /* The HSE oscillator has to be running before stm32_clock_init().*/
    palSetLine(LINE_HSE_EN);
}

/*===========================================================================*/
/* Driver interrupt handlers.                                                */
/*===========================================================================*/

/*===========================================================================*/
/* Driver exported functions.                                                */
/*===========================================================================*/

/**
 * @brief   Early initialization code.
 * @details GPIO ports and system clocks are initialized before everything
 *          else.
 */
void __early_init(void) {

    stm32_d2_sram_init();
    stm32_gpio_init();
    stm32_clock_init();
}

/**
 * @brief   Board-specific initialization code.
 */
void boardInit(void) {}
