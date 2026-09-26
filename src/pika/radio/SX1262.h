#pragma once

#include <hal.h>

#include <span>

namespace pika::radio {

struct RadioStats {
    float signal_level;
    float noise_level;
};

class SX1262 {
public:
    enum class Mode {
        CW = 0,
        GFSK,
        LORA,
        FLRC,
    };

    SX1262(SPIDriver &spi, ioline_t gpio_reset, ioline_t gpio_busy, ioline_t gpio_dio1, ioline_t gpio_tx,
           ioline_t gpio_rx) :
            _spi(spi), _gpio_reset(gpio_reset), _gpio_busy(gpio_busy), _gpio_dio1(gpio_dio1), _gpio_tx(gpio_tx),
            _gpio_rx(gpio_rx) {}

    /**
         * Init driver
         * @param payload_len payload length, > 0 for fixed length, 0 for variable length
         */
    void init(size_t payload_len);

    /**
         * Set center frequency in Hz
         * @param freq
         */
    void set_freq(uint64_t freq);

    /**
         * Set air bitrate in bits/s
         * @param bitrate
         */
    void set_mod_params(uint8_t sf, uint8_t bw, uint8_t cr);

    /**
         * Set output power in dBm
         * @param power
         */
    void set_power(float power);

    void tx(std::span<const uint8_t> data);

    std::span<const uint8_t> rx(systime_t &timestamp, systime_t end_time);

    RadioStats get_stats() const;

private:
    static constexpr uint32_t F_XTAL = 32000000;
    static constexpr double F_STEP = (double) F_XTAL / (1 << 25);
    static constexpr size_t FIFO_SIZE = 256;

    SPIDriver &_spi;
    ioline_t _gpio_reset;
    ioline_t _gpio_busy;
    ioline_t _gpio_dio1;
    ioline_t _gpio_tx;
    ioline_t _gpio_rx;

    binary_semaphore_t _sem_dio1;
    /* The payload buffer, in AHB SRAM2 through the linker's .nocache section:
       SPI transfers go out over DMA, and the D-cache is on, so a plain .bss
       buffer would have the CPU and the controller looking at different
       bytes. Static because the section is shared and there is one radio. */
    static std::array<uint8_t, FIFO_SIZE + 3> _spi_buf;
    size_t _spi_buf_size = 0;
    size_t _payload_len = 0;
    uint64_t _freq = 868000000;
    uint8_t _sf = 9;
    uint8_t _bw = 5;
    uint8_t _cr = 5;
    float _power = 0.0f;
    bool _inited = false;
    uint8_t _rssi_packet_i = 0;
    uint8_t _rssi_floor_i = 0;

    uint8_t *command_params() { return &_spi_buf[1]; };

    void write_reg(uint16_t reg, uint8_t value);

    uint8_t read_reg(uint16_t reg);

    void write_buffer(std::span<const uint8_t> data);

    std::span<uint8_t> read_buffer(size_t n);

    void do_set_freq();

    void do_set_mod_params();

    void do_set_power();

    uint8_t get_status();

    uint16_t get_irq_status();

    void get_packet_status();

    void get_rssi_inst();

    void send_command(uint8_t opcode) { send_command_n(opcode, 0); }

    void send_command(uint8_t opcode, uint8_t param0) {
        command_params()[0] = param0;
        send_command_n(opcode, 1);
    }

    void send_command(uint8_t opcode, uint8_t param0, uint8_t param1) {
        command_params()[0] = param0;
        command_params()[1] = param1;
        send_command_n(opcode, 2);
    }

    void send_command_n(uint8_t opcode, size_t param_size);

    void wait_busy();

    void enable_irq(uint8_t flags);

    static void extcb_dio1(void *ptr) {
        chSysLockFromISR();
        chBSemSignalI(&reinterpret_cast<SX1262 *>(ptr)->_sem_dio1);
        chSysUnlockFromISR();
    };
};

}// namespace pika::radio
