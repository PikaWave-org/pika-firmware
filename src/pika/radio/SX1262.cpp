#include "SX1262.h"

#include <pika/util/int_util.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pika::radio {

__attribute__((section(".nocache"), aligned(4))) std::array<uint8_t, SX1262::FIFO_SIZE + 3> SX1262::_spi_buf;

enum SX1262Commands : uint8_t {
    CMD_CLEAR_IRQ_STATUS = 0x02,
    CMD_SET_DIO_IRQ_PARAMS = 0x08,
    CMD_WRITE_REGISTER = 0x0D,
    CMD_WRITE_BUFFER = 0x0E,
    CMD_GET_IRQ_STATUS = 0x12,
    CMD_GET_PACKET_STATUS = 0x14,
    CMD_GET_RSSI_INST = 0x15,
    CMD_READ_REGISTER = 0x1D,
    CMD_READ_BUFFER = 0x1E,
    CMD_SET_STANDBY = 0x80,
    CMD_SET_RX = 0x82,
    CMD_SET_TX = 0x83,
    CMD_SET_RF_FREQUENCY = 0x86,
    CMD_SET_PACKET_TYPE = 0x8A,
    CMD_SET_MODULATION_PARAMS = 0x8B,
    CMD_SET_PACKET_PARAMS = 0x8C,
    CMD_SET_TX_PARAMS = 0x8E,
    CMD_SET_BUFFER_BASE_ADDRESS = 0x8F,
    CMD_SET_PA_CONFIG = 0x95,
    CMD_GET_STATUS = 0xC0,
    CMD_SET_TX_CONTINUOUS_WAVE = 0xD1,
    CMD_SET_DIO3_AS_TCXO_CTRL = 0x97,
    CMD_CALIBRATE_IMAGE = 0x98,
    CMD_SET_DIO2_AS_RF_SWITCH_CTRL = 0x9D,
};

enum SX1262Registers : uint16_t {
    REG_SYNC_WORD = 0x06C0,
};

void SX1262::init(size_t payload_len) {
    chBSemObjectInit(&_sem_dio1, true);

    _payload_len = payload_len;
    // Reset using GPIO
    palClearLine(_gpio_reset);
    chThdSleepMilliseconds(1);
    palSetLine(_gpio_reset);
    chThdSleepMilliseconds(6);

    palEnableLineEvent(_gpio_dio1, PAL_EVENT_MODE_RISING_EDGE);
    palSetLineCallback(_gpio_dio1, extcb_dio1, this);

    // TXCO voltage = 1.8V
    command_params()[0] = 0x02;
    command_params()[1] = 0x00;
    command_params()[2] = 0x00;
    command_params()[3] = 0x08;
    send_command_n(CMD_SET_DIO3_AS_TCXO_CTRL, 4);
    send_command(CMD_SET_DIO2_AS_RF_SWITCH_CTRL, 1);
    send_command(CMD_SET_PACKET_TYPE, 1);// LORA

    // write_reg(REG_SYNC_WORD + 0, 0x1F);
    // write_reg(REG_SYNC_WORD + 1, 0x35);

    command_params()[0] = 0x00;       // Preamble length MSB
    command_params()[1] = 0x40;       // Preamble length LSB
    command_params()[2] = 0x00;       // Header type
    command_params()[3] = payload_len;// Payload length
    command_params()[4] = 0x00;       // CRC type
    command_params()[5] = 0x00;       // Invert IQ
    command_params()[6] = 0x00;
    command_params()[7] = 0x00;
    command_params()[8] = 0x00;
    send_command_n(CMD_SET_PACKET_PARAMS, 9);

    send_command(CMD_SET_BUFFER_BASE_ADDRESS, 0, 0);

    send_command(CMD_SET_STANDBY, 1);// STDBY_XOSC

    do_set_mod_params();
    do_set_freq();
    do_set_power();

    _inited = true;
}

void SX1262::tx(std::span<const uint8_t> data) {
    write_buffer(data);

    // Enable IRQ on TxDone
    enable_irq(0x01);

    // Clear semaphore
    chBSemWaitTimeout(&_sem_dio1, TIME_IMMEDIATE);

    // Enable PA
    palSetLine(_gpio_tx);

    // Start TX
    command_params()[0] = 0x00;
    command_params()[1] = 0x00;
    command_params()[2] = 0x00;
    send_command_n(CMD_SET_TX, 3);// TX

    // Wait for TxDone interrupt
    if (!chBSemWaitTimeout(&_sem_dio1, TIME_S2I(1))) {
        // Switch to Standby
        send_command(CMD_SET_STANDBY, 0);// STDBY_RC
    }

    // Disable PA
    palClearLine(_gpio_tx);

    // Clear IRQ
    send_command(CMD_CLEAR_IRQ_STATUS, 0x00, 0x01);
}

std::span<const uint8_t> SX1262::rx(systime_t &timestamp, systime_t end_time) {
    // Enable IRQ on RxDone
    enable_irq(0x02);

    // Clear semaphore
    chBSemWaitTimeout(&_sem_dio1, TIME_IMMEDIATE);

    // Enable LNA
    palSetLine(_gpio_rx);

    // Start RX
    command_params()[0] = 0x00;
    command_params()[1] = 0x00;
    command_params()[2] = 0x00;
    send_command_n(CMD_SET_RX, 3);// RX

    // Wait before measuring noise floor
    chThdSleepMilliseconds(1);
    get_rssi_inst();

    // Wait for RxDone interrupt
    int32_t dt = end_time - chVTGetSystemTime();
    if (dt >= 0) {
        chBSemWaitTimeout(&_sem_dio1, dt);
    }
    timestamp = chVTGetSystemTime();

    // Switch to Standby
    send_command(CMD_SET_STANDBY, 1);// STDBY_XOSC

    // Disable LNA
    palClearLine(_gpio_rx);

    std::span<uint8_t> res{};
    if (get_irq_status() & 0x02) {
        // Packet received
        res = read_buffer(_payload_len);
    }

    // Clear IRQ
    send_command(CMD_CLEAR_IRQ_STATUS, 0x00, 0x02);

    // Get packet status
    get_packet_status();

    return res;
}

/*
 * Every transfer below assembles its frame in _spi_buf rather than in a local
 * array, because SPI on this MCU is DMA only and DMA cannot read the caller's
 * stack: thread stacks are in DTCM, which no DMA controller here can reach at
 * all - the transfer errors out and ChibiOS halts the system with "DMA
 * failure". _spi_buf is in .nocache (AHB SRAM2), which DMA reaches and the
 * MPU keeps out of the D-cache.
 */
void SX1262::write_reg(uint16_t reg, uint8_t value) {
    wait_busy();
    _spi_buf[0] = CMD_WRITE_REGISTER;
    _spi_buf[1] = (uint8_t) (reg >> 8);
    _spi_buf[2] = (uint8_t) reg;
    _spi_buf[3] = value;
    spiSelect(&_spi);
    spiSend(&_spi, 4, _spi_buf.data());
    spiUnselect(&_spi);
}

uint8_t SX1262::read_reg(uint16_t reg) {
    wait_busy();
    _spi_buf[0] = CMD_READ_REGISTER;
    _spi_buf[1] = (uint8_t) (reg >> 8);
    _spi_buf[2] = (uint8_t) reg;
    _spi_buf[3] = 0;
    _spi_buf[4] = 0;
    spiSelect(&_spi);
    spiExchange(&_spi, 5, _spi_buf.data(), _spi_buf.data());
    spiUnselect(&_spi);
    return _spi_buf[4];
}

void SX1262::write_buffer(std::span<const uint8_t> data) {
    wait_busy();
    _spi_buf[0] = CMD_WRITE_BUFFER;
    _spi_buf[1] = 0;// offset
    std::copy(data.begin(), data.end(), &_spi_buf[2]);
    _spi_buf_size = 2 + data.size();
    spiSelect(&_spi);
    spiSend(&_spi, _spi_buf_size, _spi_buf.data());
    spiUnselect(&_spi);
}

std::span<uint8_t> SX1262::read_buffer(size_t n) {
    wait_busy();
    _spi_buf_size = 3 + n;
    memset(_spi_buf.data(), 0, _spi_buf_size);
    _spi_buf[0] = CMD_READ_BUFFER;
    _spi_buf[1] = 0;// offset
    spiSelect(&_spi);
    spiExchange(&_spi, _spi_buf_size, _spi_buf.data(), _spi_buf.data());
    spiUnselect(&_spi);
    return {&_spi_buf[3], n};
}

void SX1262::do_set_freq() {
    uint32_t freq_v = (uint32_t) ((double) _freq / F_STEP);
    *reinterpret_cast<uint32_t *>(command_params()) = util::to_big_endian<uint32_t>(freq_v);
    send_command_n(CMD_SET_RF_FREQUENCY, 4);

    static uint32_t freq_bands[5][2] = {
            {430000000}, {470000000}, {779000000}, {863000000}, {902000000},
    };
    static uint8_t freq_band_values[5][2] = {
            {0x6B, 0x6F}, {0x75, 0x81}, {0xC1, 0xC5}, {0xD7, 0xDB}, {0xE1, 0xE9},
    };

    uint8_t *cal_values = freq_band_values[0];
    for (int i = 0; i < 5; ++i) {
        if (_freq < freq_bands[i][0]) {
            break;
        }
        cal_values = freq_band_values[i];
    }
    send_command(CMD_CALIBRATE_IMAGE, cal_values[0], cal_values[1]);
}

void SX1262::do_set_mod_params() {
    command_params()[0] = _sf;
    command_params()[1] = _bw;
    command_params()[2] = _cr;
    command_params()[3] = 0;
    command_params()[4] = 0;
    command_params()[5] = 0;
    command_params()[6] = 0;
    command_params()[7] = 0;
    send_command_n(CMD_SET_MODULATION_PARAMS, 8);
}

void SX1262::do_set_power() {
    command_params()[0] = 0x04;// paDutyCycle
    command_params()[1] = 0x07;// hpMax
    command_params()[2] = 0x00;// deviceSel
    command_params()[3] = 0x01;// paLut
    send_command_n(CMD_SET_PA_CONFIG, 4);
    uint8_t pwr_v = std::clamp((int) lroundf(_power), -9, 22);
    send_command(CMD_SET_TX_PARAMS, pwr_v, 0x01);
}

void SX1262::set_freq(uint64_t freq) {
    _freq = freq;
    if (_inited) {
        do_set_freq();
    }
}

void SX1262::set_mod_params(uint8_t sf, uint8_t bw, uint8_t cr) {
    _sf = sf;
    _bw = bw;
    _cr = cr;
    if (_inited) {
        do_set_mod_params();
    }
}

void SX1262::set_power(float power) {
    _power = power;
    if (_inited) {
        do_set_power();
    }
}

void SX1262::send_command_n(uint8_t opcode, size_t param_size) {
    wait_busy();
    _spi_buf[0] = opcode;
    spiSelect(&_spi);
    spiSend(&_spi, 1 + param_size, _spi_buf.data());
    spiUnselect(&_spi);
}

void SX1262::wait_busy() {
    while (palReadLine(_gpio_busy)) { chThdSleepMicroseconds(100); }
}

void SX1262::enable_irq(uint8_t flags) {
    command_params()[0] = 0x00;
    command_params()[1] = flags;
    command_params()[2] = 0x00;
    command_params()[3] = flags;
    command_params()[4] = 0x00;
    command_params()[5] = 0x00;
    command_params()[6] = 0x00;
    command_params()[7] = 0x00;
    send_command_n(CMD_SET_DIO_IRQ_PARAMS, 8);
}

uint8_t SX1262::get_status() {
    wait_busy();
    _spi_buf[0] = CMD_GET_STATUS;
    _spi_buf[1] = 0;
    spiSelect(&_spi);
    spiExchange(&_spi, 2, _spi_buf.data(), _spi_buf.data());
    spiUnselect(&_spi);
    return _spi_buf[1];
}

uint16_t SX1262::get_irq_status() {
    wait_busy();
    memset(_spi_buf.data(), 0, 4);
    _spi_buf[0] = CMD_GET_IRQ_STATUS;
    spiSelect(&_spi);
    spiExchange(&_spi, 4, _spi_buf.data(), _spi_buf.data());
    spiUnselect(&_spi);
    return util::from_big_endian(*reinterpret_cast<uint16_t *>(&_spi_buf[2]));
}

void SX1262::get_packet_status() {
    wait_busy();
    memset(_spi_buf.data(), 0, 5);
    _spi_buf[0] = CMD_GET_PACKET_STATUS;
    spiSelect(&_spi);
    spiExchange(&_spi, 5, _spi_buf.data(), _spi_buf.data());
    spiUnselect(&_spi);
    _rssi_packet_i = _spi_buf[4];
}

void SX1262::get_rssi_inst() {
    wait_busy();
    memset(_spi_buf.data(), 0, 3);
    _spi_buf[0] = CMD_GET_RSSI_INST;
    spiSelect(&_spi);
    spiExchange(&_spi, 3, _spi_buf.data(), _spi_buf.data());
    spiUnselect(&_spi);
    _rssi_floor_i = _spi_buf[2];
}

RadioStats SX1262::get_stats() const {
    RadioStats stats{
            .signal_level = -(float) _rssi_packet_i / 2,
            .noise_level = -(float) _rssi_floor_i / 2,
    };
    return stats;
}

}// namespace pika::radio
