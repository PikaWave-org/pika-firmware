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

extern char __ram4_base__[], __ram4_size__[];
extern char __ram_sd_base__[], __ram_sd_size__[];

/* Makes a memory region non-cacheable. Size must be a power of two.*/
static void mpu_nocache_region(uint32_t region, char *base, char *size) {
    mpuConfigureRegion(region, (uint32_t) base,
                       MPU_RASR_ATTR_AP_RW_RW | MPU_RASR_ATTR_NON_CACHEABLE | MPU_RASR_ATTR_S |
                               MPU_RASR_SIZE(__builtin_ctz((uint32_t) size) - 1U) | MPU_RASR_ENABLE);
}

/*
 * D2 SRAM (0x30000000) has its own clock gates and comes out of reset with
 * them off, so the memory silently ignores accesses until they are enabled.
 * The .nocache DMA buffers live there, so this has to happen before any of
 * it is touched. The section is NOLOAD, so startup never writes to it.
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

    /* Microphone: MIC_IN is ADC1_INP3 and stays analog, MIC_SHDN is the
     preamp's shutdown input and is active high - the reverse of SPK_EN above.
     Set it before the pin becomes an output, for the same reason the speaker
     clears its line first: a low glitch here powers the microphone up while
     nothing is ready to read it.*/
    palSetLine(LINE_MIC_SHDN);
    palSetLineMode(LINE_MIC_SHDN, PAL_MODE_OUTPUT_PUSHPULL);
    palSetLineMode(LINE_MIC_IN, PAL_MODE_INPUT_ANALOG);

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

    /* SX1262 radio on SPI2. NSS is a GPIO the SPI driver toggles, so it is set
     high before it becomes an output: a low chip select while the bus is
     still idle starts a command the radio never sees the end of. RST is
     active low and stays low here, holding the chip in reset until the
     driver's init() pulses it; RXEN/TXEN are active high and start low, with
     neither front end path enabled.*/
    palSetLine(LINE_RADIO_SPI_NSS);
    palSetLineMode(LINE_RADIO_SPI_NSS, PAL_MODE_OUTPUT_PUSHPULL | PAL_STM32_OSPEED_HIGHEST);
    palSetLineMode(LINE_RADIO_SPI_SCK, BOARD_RADIO_SPI_PINMODE);
    palSetLineMode(LINE_RADIO_SPI_MISO, BOARD_RADIO_SPI_PINMODE);
    palSetLineMode(LINE_RADIO_SPI_MOSI, BOARD_RADIO_SPI_PINMODE);
    palClearLine(LINE_RADIO_RST);
    palSetLineMode(LINE_RADIO_RST, PAL_MODE_OUTPUT_PUSHPULL);
    palClearLine(LINE_RADIO_RXEN);
    palSetLineMode(LINE_RADIO_RXEN, PAL_MODE_OUTPUT_PUSHPULL);
    palClearLine(LINE_RADIO_TXEN);
    palSetLineMode(LINE_RADIO_TXEN, PAL_MODE_OUTPUT_PUSHPULL);
    palSetLineMode(LINE_RADIO_BUSY, PAL_MODE_INPUT);
    palSetLineMode(LINE_RADIO_DIO1, PAL_MODE_INPUT);

    /* BQ25895 charger on I2C4, AF4. SCL, SDA and INT are pulled up on the
     board, so no internal pull-ups.*/
    palSetLineMode(LINE_CHG_I2C_SCL, BOARD_CHG_I2C_PINMODE);
    palSetLineMode(LINE_CHG_I2C_SDA, BOARD_CHG_I2C_PINMODE);
    palSetLineMode(LINE_CHG_INT, PAL_MODE_INPUT);

    /* USB3300 PHY on the OTG_HS ULPI port, AF10. RESET is active high and is
     cleared before the pin becomes an output: the PHY runs, and is not
     back-fed through the pad while VBUS is absent and it has no supply.*/
    palClearLine(LINE_USB_PHY_RESET);
    palSetLineMode(LINE_USB_PHY_RESET, PAL_MODE_OUTPUT_PUSHPULL);
    palSetLineMode(LINE_USB_ULPI_CK, BOARD_USB_ULPI_PINMODE);
    palSetLineMode(LINE_USB_ULPI_DIR, BOARD_USB_ULPI_PINMODE);
    palSetLineMode(LINE_USB_ULPI_NXT, BOARD_USB_ULPI_PINMODE);
    palSetLineMode(LINE_USB_ULPI_STP, BOARD_USB_ULPI_PINMODE);
    palSetLineMode(LINE_USB_ULPI_D0, BOARD_USB_ULPI_PINMODE);
    palSetLineMode(LINE_USB_ULPI_D1, BOARD_USB_ULPI_PINMODE);
    palSetLineMode(LINE_USB_ULPI_D2, BOARD_USB_ULPI_PINMODE);
    palSetLineMode(LINE_USB_ULPI_D3, BOARD_USB_ULPI_PINMODE);
    palSetLineMode(LINE_USB_ULPI_D4, BOARD_USB_ULPI_PINMODE);
    palSetLineMode(LINE_USB_ULPI_D5, BOARD_USB_ULPI_PINMODE);
    palSetLineMode(LINE_USB_ULPI_D6, BOARD_USB_ULPI_PINMODE);
    palSetLineMode(LINE_USB_ULPI_D7, BOARD_USB_ULPI_PINMODE);

    /* microSD on SDMMC1.*/
    palSetLineMode(LINE_SD_CK, BOARD_SD_CK_PINMODE);
    palSetLineMode(LINE_SD_CMD, BOARD_SD_PINMODE);
    palSetLineMode(LINE_SD_D0, BOARD_SD_PINMODE);
    palSetLineMode(LINE_SD_D1, BOARD_SD_PINMODE);
    palSetLineMode(LINE_SD_D2, BOARD_SD_PINMODE);
    palSetLineMode(LINE_SD_D3, BOARD_SD_PINMODE);

    /* The HSE oscillator has to be running before stm32_clock_init().*/
    palSetLine(LINE_HSE_EN);
}

/*===========================================================================*/
/* Driver interrupt handlers.                                                */
/*===========================================================================*/

/*===========================================================================*/
/* Driver exported functions.                                                */
/*===========================================================================*/

#if HAL_USE_SDC || defined(__DOXYGEN__)
/* No card detect: always report a card.*/
bool sdc_lld_is_card_inserted(SDCDriver *sdcp) {
    (void) sdcp;
    return true;
}

/* microSD has no write protect.*/
bool sdc_lld_is_write_protected(SDCDriver *sdcp) {
    (void) sdcp;
    return false;
}
#endif

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
void boardInit(void) {
    /* Non-cacheable DMA memory: SRAM4 for the BDMA, ram_sd for SDMMC1.*/
    mpu_nocache_region(MPU_REGION_5, __ram4_base__, __ram4_size__);
    mpu_nocache_region(MPU_REGION_4, __ram_sd_base__, __ram_sd_size__);
    SCB_CleanInvalidateDCache();
}
