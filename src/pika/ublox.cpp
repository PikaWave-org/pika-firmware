/**
 * @file ublox.cpp
 * Driver for a u-blox GNSS receiver connected via serial port.
 */

#include "ch.h"
#include "hal.h"

#include "log.h"
#include "ublox.h"

#include <cstdlib>
#include <cstring>

namespace pika::gnss {

// Sync bytes
static constexpr uint8_t UBX_SYNC1 = 0xB5;
static constexpr uint8_t UBX_SYNC2 = 0x62;

const uint32_t Ublox::_baudrates_list[8] = {9600,   19200,  38400,  57600,
                                            115200, 230400, 460800, 921600};

#pragma pack(push, 1)

// TX CFG-PRT
struct msg_tx_cfg_prt_s {
  uint8_t portID;    ///< Port Identifier Number (= 1 or 2 for UART ports)
  uint8_t res0;      ///< Reserved
  uint16_t txReady;  ///< reserved (set to 0) up to Firmware 7.00, TX ready PIN
                     ///< configuration (since Firmware 7. 00)
  uint32_t mode;     ///< A bit mask describing the UART mode
  uint32_t baudRate; ///< Baudrate in bits/second
  uint16_t inProtoMask; ///< A mask describing which input protocols are active.
  uint16_t
      outProtoMask; ///< A mask describing which output protocols are active.
  uint16_t flags;   ///< Reserved, set to 0
  uint16_t res5;    ///< Reserved, set to 0
};

// TX CFG-RATE
struct msg_tx_cfg_rate_s {
  uint16_t measRate; ///< The elapsed time between GNSS measurements [ms]
  uint16_t navRate;  ///< The ratio between the number of measurements and the
                     ///< number of navigation solutions [cycles]
  uint16_t timeRef;  ///< The time system to which measurements are aligned: 0:
                     ///< UTC time, 1: GPS time
};

// TX CFG-NAV5
struct msg_tx_cfg_nav5_s {
  uint8_t mask[2];  ///< Parameters Bitmask. Only the masked parameters will be
                    ///< applied.
  uint8_t dynModel; ///< Dynamic platform model
  uint8_t
      fixMode; ///< Position Fixing Mode: 1: 2D only, 2: 3D only, 3: auto 2D/3D
  int32_t fixedAlt; ///< Fixed altitude (mean sea level) for 2D fix mode. [m]
  uint32_t fixedAltVar; ///< Fixed altitude variance for 2D mode. [0.0001 m^2]
  int8_t minElev;  ///< Minimum Elevation for a GNSS satellite to be used in NAV
                   ///< [deg]
  uint8_t drLimit; ///< Reserved [s]
  uint16_t pDop;   ///< Position DOP Mask to use [0.1]
  uint16_t tDop;   ///< Time DOP Mask to use [0.1]
  uint16_t pAcc;   ///< Position Accuracy Mask [m]
  uint16_t tAcc;   ///< Time Accuracy Mask [m]
  uint8_t staticHoldThresh; ///< Static hold threshold [cm/s]
  uint8_t dgpsTimeOut;      ///< DGPS timeout. [s]
  uint8_t cnoThreshNumSVs; ///< Number of satellites required to have C/N0 above
                           ///< cnoThresh for a fix to be attempted
  uint8_t cnoThresh; ///< C/N0 threshold for deciding whether to attempt a fix
                     ///< [dbHz]
  uint8_t reserved1[2];       ///< Reserved
  uint16_t staticHoldMaxDist; ///< Static hold distance threshold (before
                              ///< quitting static hold) [m]
  uint8_t utcStandard;        ///< UTC standard to be used
  uint8_t reserved2[5];       ///< Reserved
};

// TX CFG-MSG
struct msg_tx_cfg_msg_s {
  uint8_t msgClass; ///< Message Class
  uint8_t msgID;    ///< Message Identifier
  uint8_t rate[6];  ///< Send rate on I/O Port (6 Ports)
};

// RX MON-VER
struct msg_rx_mon_ver_s {
  char swVersion[30]; ///< Zero-terminated Software Version String
  char hwVersion[10]; ///< Zero-terminated Hardware Version String
};

#pragma pack(pop)

Ublox::Ublox(const Config &cfg) : _cfg(cfg) {
  /* Runs before halInit(), so nothing here may touch the hardware. */
  chMtxObjectInit(&_lock);
}

void Ublox::run() {
  configure();

  // Main loop
  for (;;) {
    Message *msg;

    if (read_message(msg)) {
      handle_message(msg);
    }
  }
}

bool Ublox::nav_pvt(msg_rx_nav_pvt_s &msg) const {
  chMtxLock(&_lock);
  bool got = _got_nav_pvt;

  if (got) {
    msg = _nav_pvt;
  }

  chMtxUnlock(&_lock);

  return got;
}

bool Ublox::mon_hw(msg_rx_mon_hw_s &msg) const {
  chMtxLock(&_lock);
  bool got = _got_mon_hw;

  if (got) {
    msg = _mon_hw;
  }

  chMtxUnlock(&_lock);

  return got;
}

/*
 * sdStart() keeps the pointer rather than copying the config, so it has to
 * outlive the call. sdStop() empties both queues, which is what is wanted on
 * a rate change: bytes captured at the old speed only ever decode as noise.
 */
void Ublox::set_baudrate(uint32_t baudrate) {
  static SerialConfig serial_cfg;

  sdStop(_cfg.port);

  serial_cfg.speed = baudrate;
  serial_cfg.cr1 = 0;
  serial_cfg.cr2 = USART_CR2_STOP1_BITS;
  serial_cfg.cr3 = 0;

  sdStart(_cfg.port, &serial_cfg);
}

void Ublox::flush_buffers() {
  uint8_t discard[64];

  while (sdAsynchronousRead(_cfg.port, discard, sizeof(discard)) > 0) {
  }

  _fill = 0;
  _pos = 0;
}

Ublox::HWProtocolVersion Ublox::probe() {
  send_message(MessageClass::MON, MessageID::MON_VER, nullptr, 0);
  HWProtocolVersion version;
  Message *msg;

  if (wait_for_message(RECV_VER_TIMEOUT, msg, MessageClass::MON,
                       MessageID::MON_VER)) {
    msg_rx_mon_ver_s *mon_ver =
        reinterpret_cast<msg_rx_mon_ver_s *>(msg->payload());

    LOG("gnss: MON_VER.swVersion: %s", mon_ver->swVersion);
    LOG("gnss: MON_VER.hwVersion: %s", mon_ver->hwVersion);

    if (!strncmp("00080000", mon_ver->hwVersion, sizeof(mon_ver->hwVersion))) {
      version.hwVersion = HWVersion::UBLOX8;
    } else if (!strncmp("00190000", mon_ver->hwVersion,
                        sizeof(mon_ver->hwVersion))) {
      version.hwVersion = HWVersion::UBLOX9;
    } else {
      version.hwVersion = HWVersion::UNKNOWN;
    }

    version.protocol = 0;
    static const size_t ext_size = 30;

    for (size_t idx = 40; idx < msg->length; idx += ext_size) {
      char *ext = reinterpret_cast<char *>(msg->payload()) + idx;

      if (!strncmp("PROTVER", ext, 7)) {
        version.protocol = strtol(ext + 8, nullptr, 10);
      }
    }

    LOG("gnss: found device: %s, protocol: %d",
        hw_version_string(version.hwVersion), version.protocol);
  } else {
    LOG("gnss: device not found");
  }

  return version;
}

bool Ublox::read_message(Message *&msg) {
  for (;;) {
    // Try to parse message from buffer
    while (parse_buf().size() >= sizeof(Message) + sizeof(Checksum)) {
      std::span<uint8_t> buf = parse_buf();
      msg = reinterpret_cast<Message *>(buf.data());

      // Check sync bytes
      if (!(msg->sync1 == UBX_SYNC1 && msg->sync2 == UBX_SYNC2)) {
        if (_sync) {
          _sync = false;
          _errors++;
        }

        _pos += 1;
        continue;
      }

      // Check message length
      size_t full_msg_length = msg->length + sizeof(Message) + sizeof(Checksum);

      if (full_msg_length > BUFFER_SIZE) {
        // Longer than the buffer can ever hold, so it cannot be ours
        _errors++;
        _pos += 1;
        continue;
      }

      if (buf.size() < full_msg_length) {
        break; // Buffer underflow
      }

      // Verify checksum
      Checksum checksum_calc =
          calculate_checksum(buf.subspan(2, msg->length + sizeof(Message) - 2));
      Checksum *checksum =
          reinterpret_cast<Checksum *>(&buf[msg->length + sizeof(Message)]);

      if (checksum_calc.ck_a != checksum->ck_a ||
          checksum_calc.ck_b != checksum->ck_b) {
        LOG("gnss: checksum error: %02x != %02x || %02x != %02x",
            checksum_calc.ck_a, checksum->ck_a, checksum_calc.ck_b,
            checksum->ck_b);
        _errors++;
        _pos += full_msg_length;
        continue;
      }

      _sync = true;
      _frames++;
      _pos += full_msg_length;

      return true; // msg points to the start of the message
    }

    // Buffer underflow, make room and read more data from the port
    if (_pos > 0) {
      memmove(_buffer, &_buffer[_pos], _fill - _pos);
      _fill -= _pos;
      _pos = 0;
    }

    if (_fill == BUFFER_SIZE) {
      LOG("gnss: buffer overflow");
      _errors++;
      _fill = 0;
    }

    // Sleep some time to wait for data
    chThdSleep(RECV_TIMEOUT);

    // Read data
    size_t n =
        sdAsynchronousRead(_cfg.port, &_buffer[_fill], BUFFER_SIZE - _fill);

    if (n == 0) {
      // No data available
      return false;
    }

    _fill += n;
    // Try to parse again
  }
}

bool Ublox::wait_for_message(sysinterval_t timeout, Message *&msg,
                             MessageClass msg_class, MessageID msg_id) {
  systime_t start = chVTGetSystemTimeX();

  for (;;) {
    if (read_message(msg)) {
      if (msg->msg_class == static_cast<uint8_t>(msg_class) &&
          (msg_id == MessageID::ANY ||
           msg->msg_id == static_cast<uint8_t>(msg_id))) {
        return true; // Specified message received
      }
    }

    // Try to receive again if time left
    if (chVTTimeElapsedSinceX(start) >= timeout) {
      return false; // Timeout
    }
  }
}

bool Ublox::send_message(MessageClass msg_class, MessageID msg_id,
                         const void *payload, size_t payload_length,
                         bool wait_ack) {
  uint8_t buffer[sizeof(Message) + MAX_TX_PAYLOAD + sizeof(Checksum)];
  size_t packet_size = sizeof(Message) + payload_length + sizeof(Checksum);

  if (packet_size > sizeof(buffer)) {
    return false;
  }

  Message *header = reinterpret_cast<Message *>(buffer);
  header->sync1 = UBX_SYNC1;
  header->sync2 = UBX_SYNC2;
  header->msg_class = static_cast<uint8_t>(msg_class);
  header->msg_id = static_cast<uint8_t>(msg_id);
  header->length = static_cast<uint16_t>(payload_length);

  if (payload_length > 0) {
    memcpy(buffer + sizeof(Message), payload, payload_length);
  }

  Checksum *checksum =
      reinterpret_cast<Checksum *>(buffer + sizeof(Message) + payload_length);
  *checksum =
      calculate_checksum({buffer + 2, packet_size - 2 - sizeof(Checksum)});

  send_data(buffer, packet_size);

  if (wait_ack) {
    Message *ack_msg;

    if (wait_for_message(RECV_ACK_TIMEOUT, ack_msg, MessageClass::ACK,
                         MessageID::ANY)) {
      if (ack_msg->msg_id != static_cast<uint8_t>(MessageID::ACK_ACK)) {
        LOG("gnss: NAK for msg %02x %02x", static_cast<unsigned>(msg_class),
            static_cast<unsigned>(msg_id));
        return false;
      }
    } else {
      LOG("gnss: timeout waiting ACK for msg %02x %02x",
          static_cast<unsigned>(msg_class), static_cast<unsigned>(msg_id));
      return false;
    }
  }

  return true;
}

void Ublox::send_cfg_msg(MessageClass msg_class, MessageID msg_id, uint8_t rate,
                         bool wait_ack) {
  msg_tx_cfg_msg_s cfg_msg = {};
  cfg_msg.msgClass = static_cast<uint8_t>(msg_class);
  cfg_msg.msgID = static_cast<uint8_t>(msg_id);

  for (auto &r : cfg_msg.rate) {
    r = rate;
  }

  send_message(MessageClass::CFG, MessageID::CFG_MSG, &cfg_msg, sizeof(cfg_msg),
               wait_ack);
}

void Ublox::send_data(const uint8_t *data, size_t length) {
  sdWrite(_cfg.port, data, length);
}

void Ublox::configure() {
  _version = {};

  // Try different baudrates
  bool found = false;

  for (int round = 0; round < 3 && !found; ++round) {
    for (uint32_t baudrate : _baudrates_list) {
      LOG("gnss: try baudrate: %u", (unsigned)baudrate);
      set_baudrate(baudrate);
      chThdSleep(RECV_VER_TIMEOUT);
      flush_buffers();
      _version = probe();

      if (_version.found() && _version.hwVersion != HWVersion::UNKNOWN) {
        found = true;

        // Set baudrate/protocols for the port
        LOG("gnss: set baudrate: %u", (unsigned)_cfg.baudrate);

        msg_tx_cfg_prt_s cfg_prt = {};
        cfg_prt.portID = _cfg.port_id;
        cfg_prt.mode = 0x08D0; // 8N1
        cfg_prt.baudRate = _cfg.baudrate;
        cfg_prt.inProtoMask = (1 << 0);  // UBX in
        cfg_prt.outProtoMask = (1 << 0); // UBX out

        /* Not acked: the receiver changes rate as soon as it has
           parsed this, so any ACK would arrive at the new speed. */
        send_message(MessageClass::CFG, MessageID::CFG_PRT, &cfg_prt,
                     sizeof(cfg_prt));

        chThdSleepMilliseconds(50);

        set_baudrate(_cfg.baudrate);

        chThdSleepMilliseconds(50);
        flush_buffers();

        // Probe again at new settings
        _version = probe();

        break;
      }
    }
  }

  if (!_version.found()) {
    LOG("gnss: ERROR: configuration failed, device not found");
  } else {
    // Configure device
    LOG("gnss: configure as %s", hw_version_string(_version.hwVersion));

    msg_tx_cfg_rate_s cfg_rate = {};
    cfg_rate.measRate = _cfg.meas_int_ms;
    cfg_rate.navRate = 1;
    cfg_rate.timeRef = 1;
    send_message(MessageClass::CFG, MessageID::CFG_RATE, &cfg_rate,
                 sizeof(cfg_rate), true);

    msg_tx_cfg_nav5_s cfg_nav5 = {};
    cfg_nav5.mask[0] = 0x05;
    cfg_nav5.dynModel = 3;
    cfg_nav5.fixMode = 2;
    send_message(MessageClass::CFG, MessageID::CFG_NAV5, &cfg_nav5,
                 sizeof(cfg_nav5), true);

    send_cfg_msg(MessageClass::NAV, MessageID::NAV_PVT, 1, true);

    /*
     * Front-end health, at the same rate as the solution: the rate is in
     * navigation epochs, so 1 here is one MON-HW per NAV-PVT. Cheap at 60
     * bytes, and it is the only way to tell a receiver with no sky view from
     * one whose antenna is open or being jammed.
     */
    send_cfg_msg(MessageClass::MON, MessageID::MON_HW, 1, true);

    LOG("gnss: configuration done");
  }
}

void Ublox::handle_message(Message *msg) {
  void *payload = msg->payload();
  MessageClass msg_class = static_cast<MessageClass>(msg->msg_class);
  MessageID msg_id = static_cast<MessageID>(msg->msg_id);

  bool handled = true;

  switch (msg_class) {
  case MessageClass::NAV: {
    switch (msg_id) {
    case MessageID::NAV_PVT: {
      if (msg->length < sizeof(msg_rx_nav_pvt_s)) {
        // Older protocol version, fields would land elsewhere
        _errors++;
        break;
      }

      msg_rx_nav_pvt_s *ubx_pvt = reinterpret_cast<msg_rx_nav_pvt_s *>(payload);

      chMtxLock(&_lock);
      _nav_pvt = *ubx_pvt;
      _got_nav_pvt = true;
      chMtxUnlock(&_lock);

      break;
    }

    default:
      handled = false;
      break;
    }

    break;
  }

  case MessageClass::MON: {
    switch (msg_id) {
    case MessageID::MON_HW: {
      if (msg->length < sizeof(msg_rx_mon_hw_s)) {
        // Not the M8 layout, fields would land elsewhere
        _errors++;
        break;
      }

      msg_rx_mon_hw_s *ubx_hw = reinterpret_cast<msg_rx_mon_hw_s *>(payload);

      chMtxLock(&_lock);
      _mon_hw = *ubx_hw;
      _got_mon_hw = true;
      chMtxUnlock(&_lock);

      break;
    }

    default:
      handled = false;
      break;
    }

    break;
  }

  case MessageClass::ACK:
    break; // Ignore ACK

  default:
    handled = false;
    break;
  }

  if (!handled) {
    LOG("gnss: unsupported message: 0x%02x 0x%02x", msg->msg_class,
        msg->msg_id);
    send_cfg_msg(msg_class, msg_id, 0, false);
  }
}

const char *Ublox::hw_version_string(HWVersion version) {
  switch (version) {
  case HWVersion::UBLOX8:
    return "ublox8";

  case HWVersion::UBLOX9:
    return "ublox9";

  case HWVersion::NONE:
    return "NONE";

  default:
    return "UNKNOWN";
  }
}

Ublox::Checksum Ublox::calculate_checksum(std::span<const uint8_t> data) {
  Checksum checksum = {};

  for (uint8_t b : data) {
    checksum.ck_a += b;
    checksum.ck_b = checksum.ck_b + checksum.ck_a;
  }

  return checksum;
}

} /* namespace pika::gnss */
