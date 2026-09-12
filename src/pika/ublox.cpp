#include "ch.h"
#include "hal.h"

#include "ublox.h"

namespace pika::gnss {
namespace {

/* UBX frame delimiters and the fixed overhead around a payload. */
constexpr uint8_t sync1 = 0xB5U;
constexpr uint8_t sync2 = 0x62U;
constexpr uint16_t frame_overhead = 8U;  /* 2 sync, cls, id, 2 len, 2 ck. */

/* Message classes and identifiers used here. */
constexpr uint8_t cls_nav = 0x01U;
constexpr uint8_t id_nav_pvt = 0x07U;

constexpr uint8_t cls_ack = 0x05U;
constexpr uint8_t id_ack_nak = 0x00U;
constexpr uint8_t id_ack_ack = 0x01U;

constexpr uint8_t cls_cfg = 0x06U;
constexpr uint8_t id_cfg_prt = 0x00U;
constexpr uint8_t id_cfg_msg = 0x01U;
constexpr uint8_t id_cfg_rate = 0x08U;

constexpr uint8_t cls_mon = 0x0AU;
constexpr uint8_t id_mon_ver = 0x04U;

/* Parser states, in the order a frame goes through them. */
constexpr uint8_t st_sync1 = 0U;
constexpr uint8_t st_sync2 = 1U;
constexpr uint8_t st_class = 2U;
constexpr uint8_t st_id = 3U;
constexpr uint8_t st_len_lo = 4U;
constexpr uint8_t st_len_hi = 5U;
constexpr uint8_t st_payload = 6U;
constexpr uint8_t st_ck_a = 7U;
constexpr uint8_t st_ck_b = 8U;

/*
 * The receiver's own UART, as numbered in CFG-PRT. Port 0 is DDC/I2C and
 * port 4 is SPI on this module; only port 1 is brought out on the board.
 */
constexpr uint8_t port_uart1 = 1U;

/* CFG-PRT mode field: 8 data bits, no parity, 1 stop bit. */
constexpr uint32_t prt_mode_8n1 = 0x000008D0U;

/* CFG-PRT protocol masks. UBX only, which is what turns NMEA output off. */
constexpr uint16_t proto_ubx = 0x0001U;

/* CFG-RATE time reference: 1 is GPS time, 0 would be UTC. */
constexpr uint16_t rate_timeref_gps = 1U;

/* How long to wait for a reply before giving up on a configuration step. */
constexpr sysinterval_t probe_timeout = TIME_MS2I(1500);
constexpr sysinterval_t ack_timeout = TIME_MS2I(1000);

/*
 * Time for a frame to leave the shift register before the port is torn down.
 * sdWrite() returns once the bytes are queued, not once they are on the wire,
 * and CFG-PRT is the one frame that must be fully transmitted at the old rate
 * before anything changes underneath it. 100ms covers the 28 byte frame at
 * 9600 baud roughly three times over.
 */
constexpr sysinterval_t drain_time = TIME_MS2I(100);

/* Little endian readers. The payload is not aligned, so it is read bytewise. */
uint16_t rd_u16(const uint8_t *p) {

  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

uint32_t rd_u32(const uint8_t *p) {

  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int32_t rd_i32(const uint8_t *p) {

  return (int32_t)rd_u32(p);
}

} /* anonymous namespace */

Ublox::Ublox(const Config &cfg) : cfg_(cfg) {

  /* Runs before halInit(), so nothing here may touch the hardware. */
  chMtxObjectInit(&lock_);
}

/*
 * Reopens the port at a new rate. sdStop() empties both queues, which matters
 * on a rate change: bytes captured at the old speed would only ever decode as
 * garbage. The parser is restarted for the same reason.
 *
 * The configuration has to outlive this call - sdStart() keeps the pointer
 * rather than copying the structure - hence the member.
 */
void Ublox::open(uint32_t baud) {

  sdStop(cfg_.sd);

  serial_cfg_.speed = baud;
  serial_cfg_.cr1 = 0U;
  serial_cfg_.cr2 = USART_CR2_STOP1_BITS;
  serial_cfg_.cr3 = 0U;

  sdStart(cfg_.sd, &serial_cfg_);

  state_ = st_sync1;
}

void Ublox::send(uint8_t cls, uint8_t id, const uint8_t *payload,
                  uint16_t len) {

  uint8_t buf[frame_overhead + 32U];

  if (len > sizeof buf - frame_overhead) {
    return;
  }

  buf[0] = sync1;
  buf[1] = sync2;
  buf[2] = cls;
  buf[3] = id;
  buf[4] = (uint8_t)(len & 0xFFU);
  buf[5] = (uint8_t)(len >> 8);

  for (uint16_t i = 0U; i < len; i++) {
    buf[6U + i] = payload[i];
  }

  /* 8 bit Fletcher checksum over everything between the sync bytes and it. */
  uint8_t a = 0U, b = 0U;
  for (uint16_t i = 2U; i < 6U + len; i++) {
    a = (uint8_t)(a + buf[i]);
    b = (uint8_t)(b + a);
  }
  buf[6U + len] = a;
  buf[7U + len] = b;

  sdWrite(cfg_.sd, buf, (size_t)len + frame_overhead);
}

/*
 * Runs one byte through the frame parser. Returns true when a complete,
 * checksummed frame has landed in payload_.
 */
bool Ublox::feed(uint8_t b) {

  switch (state_) {
  case st_sync1:
    if (b == sync1) {
      state_ = st_sync2;
    }
    break;

  case st_sync2:
    if (b == sync2) {
      state_ = st_class;
    }
    else {
      /* Not a frame start after all; b may itself be one. */
      state_ = (b == sync1) ? st_sync2 : st_sync1;
      stats_.resyncs++;
    }
    break;

  case st_class:
    msg_cls_ = b;
    ck_a_ = b;
    ck_b_ = b;
    state_ = st_id;
    break;

  case st_id:
    msg_id_ = b;
    ck_a_ = (uint8_t)(ck_a_ + b);
    ck_b_ = (uint8_t)(ck_b_ + ck_a_);
    state_ = st_len_lo;
    break;

  case st_len_lo:
    msg_len_ = b;
    ck_a_ = (uint8_t)(ck_a_ + b);
    ck_b_ = (uint8_t)(ck_b_ + ck_a_);
    state_ = st_len_hi;
    break;

  case st_len_hi:
    msg_len_ = (uint16_t)(msg_len_ | ((uint16_t)b << 8));
    ck_a_ = (uint8_t)(ck_a_ + b);
    ck_b_ = (uint8_t)(ck_b_ + ck_a_);
    msg_pos_ = 0U;

    if (msg_len_ > max_payload) {
      /* Either a message we never asked for or a desynchronised stream.
         Either way, buffering it is not an option.*/
      stats_.resyncs++;
      state_ = st_sync1;
    }
    else {
      state_ = (msg_len_ == 0U) ? st_ck_a : st_payload;
    }
    break;

  case st_payload:
    payload_[msg_pos_++] = b;
    ck_a_ = (uint8_t)(ck_a_ + b);
    ck_b_ = (uint8_t)(ck_b_ + ck_a_);
    if (msg_pos_ >= msg_len_) {
      state_ = st_ck_a;
    }
    break;

  case st_ck_a:
    state_ = (b == ck_a_) ? st_ck_b : st_sync1;
    if (state_ == st_sync1) {
      stats_.checksum_errors++;
    }
    break;

  case st_ck_b:
    state_ = st_sync1;
    if (b == ck_b_) {
      stats_.frames_ok++;
      return true;
    }
    stats_.checksum_errors++;
    break;

  default:
    state_ = st_sync1;
    break;
  }

  return false;
}

/*
 * Acts on the frame sitting in payload_. Everything that reads the stream
 * goes through here, so a navigation solution arriving in the middle of the
 * configuration handshake is decoded rather than thrown away.
 */
void Ublox::dispatch(void) {

  if ((msg_cls_ == cls_nav) && (msg_id_ == id_nav_pvt)) {
    decode_nav_pvt();
  }
  else if (msg_cls_ == cls_ack) {
    if (msg_len_ >= 2U) {
      ack_cls_ = payload_[0];
      ack_id_ = payload_[1];
      ack_ok_ = (msg_id_ == id_ack_ack);
      ack_seen_ = true;
      if (!ack_ok_) {
        stats_.naks++;
      }
    }
  }
}

void Ublox::decode_nav_pvt(void) {

  if (msg_len_ < ubx_nav_pvt::len) {
    /* A shorter payload means an older protocol version laying the fields out
       differently; decoding it against these offsets would yield plausible
       nonsense, so it is counted and dropped instead.*/
    stats_.resyncs++;
    return;
  }

  /*
   * Offsets are the ones from the interface description, quoted here so this
   * can be checked against it line by line. Bytes 78..83 are reserved on this
   * protocol version and are skipped.
   */
  const uint8_t *p = payload_;
  ubx_nav_pvt m = {};

  m.iTOW = rd_u32(&p[0]);
  m.year = rd_u16(&p[4]);
  m.month = p[6];
  m.day = p[7];
  m.hour = p[8];
  m.min = p[9];
  m.sec = p[10];
  m.valid = p[11];
  m.tAcc = rd_u32(&p[12]);
  m.nano = rd_i32(&p[16]);

  m.fixType = p[20];
  m.flags = p[21];
  m.flags2 = p[22];
  m.numSV = p[23];

  m.lon = rd_i32(&p[24]);
  m.lat = rd_i32(&p[28]);
  m.height = rd_i32(&p[32]);
  m.hMSL = rd_i32(&p[36]);
  m.hAcc = rd_u32(&p[40]);
  m.vAcc = rd_u32(&p[44]);

  m.velN = rd_i32(&p[48]);
  m.velE = rd_i32(&p[52]);
  m.velD = rd_i32(&p[56]);
  m.gSpeed = rd_i32(&p[60]);
  m.headMot = rd_i32(&p[64]);
  m.sAcc = rd_u32(&p[68]);
  m.headAcc = rd_u32(&p[72]);

  m.pDOP = rd_u16(&p[76]);

  m.headVeh = rd_i32(&p[84]);
  m.magDec = (int16_t)rd_u16(&p[88]);
  m.magAcc = rd_u16(&p[90]);

  chMtxLock(&lock_);
  nav_pvt_ = m;
  chMtxUnlock(&lock_);

  have_nav_pvt_ = true;
}

/*
 * Reads one byte, counting it, with the wait bounded by the deadline rather
 * than restarted per byte: a receiver dribbling out a message it was not
 * asked for must not be able to extend a wait indefinitely.
 */
msg_t Ublox::get_until(systime_t deadline) {

  sysinterval_t left = chTimeDiffX(chVTGetSystemTimeX(), deadline);

  if ((int32_t)left <= 0) {
    return MSG_TIMEOUT;
  }

  msg_t m = sdGetTimeout(cfg_.sd, left);
  if (m >= MSG_OK) {
    stats_.bytes_rx++;
  }

  return m;
}

/*
 * Reads frames until one of class cls and identifier id turns up, or the
 * deadline passes. Every frame seen on the way is dispatched, so a navigation
 * solution arriving mid-handshake is decoded rather than discarded.
 */
bool Ublox::await(uint8_t cls, uint8_t id, sysinterval_t timeout) {

  systime_t deadline = chTimeAddX(chVTGetSystemTimeX(), timeout);

  for (;;) {
    msg_t m = get_until(deadline);
    if (m < MSG_OK) {
      return false;
    }

    if (feed((uint8_t)m)) {
      dispatch();
      if ((msg_cls_ == cls) && (msg_id_ == id)) {
        return true;
      }
    }
  }
}

/*
 * Waits for the receiver's acknowledgement of a configuration message. A NAK
 * is a definite answer, so it ends the wait too - reported through the return
 * value and counted in stats.
 */
bool Ublox::ack_wait(uint8_t cls, uint8_t id, sysinterval_t timeout) {

  systime_t deadline = chTimeAddX(chVTGetSystemTimeX(), timeout);

  ack_seen_ = false;

  for (;;) {
    msg_t m = get_until(deadline);
    if (m < MSG_OK) {
      return false;
    }

    if (feed((uint8_t)m)) {
      dispatch();
      if (ack_seen_ && (ack_cls_ == cls) && (ack_id_ == id)) {
        return ack_ok_;
      }
    }
  }
}

/*
 * Asks the receiver to identify itself and waits for the reply. Any valid
 * MON-VER frame proves the baud rate is right and UBX is getting through,
 * which is all this is for; the version strings themselves are ignored.
 */
bool Ublox::probe(sysinterval_t timeout) {

  send(cls_mon, id_mon_ver, nullptr, 0U);

  return await(cls_mon, id_mon_ver, timeout);
}

bool Ublox::init(void) {

  ready_ = false;
  error_ = err_none;

  /* Step 1: the receiver may already be configured from an earlier run. */
  open(cfg_.baud_run);
  bool up = probe(probe_timeout);

  if (!up) {
    /* Step 2: cold module, still at its power-on rate. CFG-PRT moves it to
       baud_run and drops NMEA from both protocol masks.*/
    open(cfg_.baud_boot);

    uint8_t prt[20] = {};
    prt[0] = port_uart1;
    prt[4] = (uint8_t)(prt_mode_8n1 & 0xFFU);
    prt[5] = (uint8_t)((prt_mode_8n1 >> 8) & 0xFFU);
    prt[6] = (uint8_t)((prt_mode_8n1 >> 16) & 0xFFU);
    prt[7] = (uint8_t)((prt_mode_8n1 >> 24) & 0xFFU);
    prt[8] = (uint8_t)(cfg_.baud_run & 0xFFU);
    prt[9] = (uint8_t)((cfg_.baud_run >> 8) & 0xFFU);
    prt[10] = (uint8_t)((cfg_.baud_run >> 16) & 0xFFU);
    prt[11] = (uint8_t)((cfg_.baud_run >> 24) & 0xFFU);
    prt[12] = (uint8_t)(proto_ubx & 0xFFU);
    prt[13] = (uint8_t)(proto_ubx >> 8);
    prt[14] = (uint8_t)(proto_ubx & 0xFFU);
    prt[15] = (uint8_t)(proto_ubx >> 8);

    send(cls_cfg, id_cfg_prt, prt, sizeof prt);

    /* No ACK is waited for here: the receiver switches rate as soon as it has
       parsed the frame, so its acknowledgement would arrive at the new speed
       and never be seen at this one. Absence of an ACK is expected, not a
       failure. All that is needed is for the frame to finish going out.*/
    chThdSleepMilliseconds((uint32_t)TIME_I2MS(drain_time));

    /* Step 3: the link should now be at the fast rate. */
    open(cfg_.baud_run);
    up = probe(probe_timeout);

    if (!up) {
      /* One retry: a module that was mid-boot when the first probe ran will
         have missed it entirely.*/
      up = probe(probe_timeout);
    }
  }

  if (!up) {
    error_ = err_no_link;
    return false;
  }

  /*
   * Probing at the wrong rate necessarily produces garbage, so the counters
   * are cleared now that the link is known good. From here on a non-zero
   * checksum or resync count means a real problem rather than the cost of
   * finding the receiver.
   */
  stats_ = {};

  /* Step 4: navigation rate. */
  uint8_t rate[6] = {};
  rate[0] = (uint8_t)(cfg_.nav_period_ms & 0xFFU);
  rate[1] = (uint8_t)(cfg_.nav_period_ms >> 8);
  rate[2] = 1U;  /* One navigation solution per measurement. */
  rate[3] = 0U;
  rate[4] = (uint8_t)(rate_timeref_gps & 0xFFU);
  rate[5] = (uint8_t)(rate_timeref_gps >> 8);

  send(cls_cfg, id_cfg_rate, rate, sizeof rate);
  if (!ack_wait(cls_cfg, id_cfg_rate, ack_timeout)) {
    error_ = err_cfg_rate;
    return false;
  }

  /* Step 5: enable UBX-NAV-PVT on the port it is asked over. */
  uint8_t msg[3] = {cls_nav, id_nav_pvt, 1U};

  send(cls_cfg, id_cfg_msg, msg, sizeof msg);
  if (!ack_wait(cls_cfg, id_cfg_msg, ack_timeout)) {
    error_ = err_cfg_msg;
    return false;
  }

  ready_ = true;

  return true;
}

bool Ublox::poll(sysinterval_t timeout) {

  have_nav_pvt_ = false;

  /* Block until the receiver says something. */
  msg_t m = sdGetTimeout(cfg_.sd, timeout);
  if (m < MSG_OK) {
    stats_.timeouts++;
    return false;
  }

  do {
    stats_.bytes_rx++;

    if (feed((uint8_t)m)) {
      dispatch();
    }

    /* Drain the rest of the burst without blocking, so a whole second's
       worth of messages is consumed in one call.*/
    m = sdGetTimeout(cfg_.sd, TIME_IMMEDIATE);
  } while (m >= MSG_OK);

  return have_nav_pvt_;
}

ubx_nav_pvt Ublox::nav_pvt(void) {

  chMtxLock(&lock_);
  ubx_nav_pvt m = nav_pvt_;
  chMtxUnlock(&lock_);

  return m;
}

} /* namespace pika::gnss */
