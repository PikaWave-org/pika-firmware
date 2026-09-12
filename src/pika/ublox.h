/*
 * Driver for a u-blox GNSS receiver on a UART, speaking the UBX binary
 * protocol.
 *
 * Nothing here is specific to one part: UBX framing, the CFG messages used to
 * set the port up and UBX-NAV-PVT are common across the u-blox generations,
 * so this is named for the protocol rather than for the receiver. The module
 * on pika_go_0 is a CAM-M8Q-0, an M8 concurrent GNSS module with an
 * integrated chip antenna, but the driver only assumes a receiver that speaks
 * UBX and that is protocol version 15 or later - which is where NAV-PVT
 * arrived, and which every M8 satisfies.
 *
 * The receiver's wiring - which serial driver it sits behind and the two baud
 * rates involved - is passed in as a Config at construction, so the driver
 * itself does not depend on any board header. The board's own BOARD_GNSS_*
 * definitions are what the caller fills the Config from.
 *
 * NMEA is switched off during init(), which halves the bytes on the wire and
 * removes the ambiguity of parsing two framings out of one stream. Position,
 * velocity and time then all arrive in a single UBX-NAV-PVT message.
 *
 * u-blox modules power up at 9600 baud; init() moves the link to whatever
 * Config asks for (see the bring-up note on init() for the ordering, which is
 * not obvious). Nothing is persisted in the receiver: without backup power
 * the settings would not survive anyway, so the sequence is simply re-run on
 * every boot.
 *
 * Usage:
 *
 *   static const pika::gnss::Ublox::Config gnss_cfg = {
 *     &BOARD_GNSS_SERIAL, BOARD_GNSS_BAUD_BOOT, BOARD_GNSS_BAUD_RUN, 1000U
 *   };
 *   static pika::gnss::Ublox gnss{gnss_cfg};
 *
 *   gnss.init();
 *   while (true) {
 *     if (gnss.poll(TIME_MS2I(1500))) {
 *       pika::gnss::ubx_nav_pvt pvt = gnss.nav_pvt();
 *     }
 *   }
 *
 * @note  poll() blocks, so it wants a thread of its own. Unlike the LCD
 *        driver, everything here lives in the instance: the serial driver's
 *        queues are ordinary SRAM, so there is no DMA placement constraint
 *        and no reason to limit the class to a single instance.
 */

#pragma once

#include <cstdint>

#include "ch.h"
#include "hal.h"

/*
 * Everything the UBX protocol defines stays at namespace scope: the message
 * and the encoding of its fields are properties of the protocol, not of this
 * driver, and pika::gnss::Ublox::ubx_nav_pvt would only stutter. What belongs
 * to the driver - its Config, its counters and its error codes - is scoped
 * inside the class, as the LCD driver does.
 */
namespace pika::gnss {

/** @brief  UBX-NAV-PVT payload length, protocol version 15 and later. */
constexpr uint16_t nav_pvt_len = 92U;

/* Values of ubx_nav_pvt::fixType. */
constexpr uint8_t fix_type_none = 0U;
constexpr uint8_t fix_type_dead_reckoning = 1U;
constexpr uint8_t fix_type_2d = 2U;
constexpr uint8_t fix_type_3d = 3U;
constexpr uint8_t fix_type_gnss_dead_reckoning = 4U;
constexpr uint8_t fix_type_time_only = 5U;

/* Bits of ubx_nav_pvt::valid. */
constexpr uint8_t valid_date = 0x01U;
constexpr uint8_t valid_time = 0x02U;
constexpr uint8_t valid_fully_resolved = 0x04U;
constexpr uint8_t valid_mag = 0x08U;

/* Bits of ubx_nav_pvt::flags. */
constexpr uint8_t flags_gnss_fix_ok = 0x01U;
constexpr uint8_t flags_diff_soln = 0x02U;
constexpr uint8_t flags_head_veh_valid = 0x20U;

/**
 * @brief   The UBX-NAV-PVT message: position, velocity and time in one.
 * @details Field names and units are u-blox's own, so this reads directly
 *          against the interface description rather than needing a mental
 *          translation step. Nothing is scaled or converted on the way in -
 *          the fixed point units are kept as sent and the caller scales once,
 *          at the point of use, so no precision is lost here.
 *
 *          This mirrors the message rather than being cast over it: the
 *          decoder reads each field by explicit offset, which keeps it
 *          independent of how the compiler would lay a packed struct out.
 *          The reserved bytes of the message are therefore not carried.
 */
struct ubx_nav_pvt {
  uint32_t iTOW;           /**< GPS time of week, milliseconds.              */

  uint16_t year;           /**< UTC year, 1999..2099.                        */
  uint8_t  month;          /**< UTC month, 1..12.                            */
  uint8_t  day;            /**< UTC day, 1..31.                              */
  uint8_t  hour;           /**< UTC hour, 0..23.                             */
  uint8_t  min;            /**< UTC minute, 0..59.                           */
  uint8_t  sec;            /**< UTC second, 0..60.                           */
  uint8_t  valid;          /**< Validity flags, the valid_* bits above.      */
  uint32_t tAcc;           /**< Time accuracy estimate, nanoseconds.         */
  int32_t  nano;           /**< Fraction of second, -1e9..1e9 nanoseconds.   */

  uint8_t  fixType;        /**< One of the fix_type_* constants above.       */
  uint8_t  flags;          /**< Fix status flags, the flags_* bits above.    */
  uint8_t  flags2;         /**< Additional flags, UTC standard and epoch.    */
  uint8_t  numSV;          /**< Satellites used in the solution.             */

  int32_t  lon;            /**< Longitude, 1e-7 degrees.                     */
  int32_t  lat;            /**< Latitude, 1e-7 degrees.                      */
  int32_t  height;         /**< Height above the ellipsoid, millimetres.     */
  int32_t  hMSL;           /**< Height above mean sea level, millimetres.    */
  uint32_t hAcc;           /**< Horizontal accuracy estimate, millimetres.   */
  uint32_t vAcc;           /**< Vertical accuracy estimate, millimetres.     */

  int32_t  velN;           /**< North velocity, millimetres per second.      */
  int32_t  velE;           /**< East velocity, millimetres per second.       */
  int32_t  velD;           /**< Down velocity, millimetres per second.       */
  int32_t  gSpeed;         /**< Ground speed, 2D, millimetres per second.    */
  int32_t  headMot;        /**< Heading of motion, 2D, 1e-5 degrees.         */
  uint32_t sAcc;           /**< Speed accuracy estimate, millimetres/second. */
  uint32_t headAcc;        /**< Heading accuracy estimate, 1e-5 degrees.     */

  uint16_t pDOP;           /**< Position DOP, 0.01 units.                    */

  /* Dead reckoning and magnetometer fields. The CAM-M8Q is a plain GNSS
     receiver, not an ADR or UDR product, so it leaves these at zero.*/
  int32_t  headVeh;        /**< Heading of vehicle, 2D, 1e-5 degrees.        */
  int16_t  magDec;         /**< Magnetic declination, 1e-2 degrees.          */
  uint16_t magAcc;         /**< Declination accuracy, 1e-2 degrees.          */
};

class Ublox {
public:

  /*
   * Largest UBX payload the parser will accept. UBX-NAV-PVT, the only message
   * enabled here, is 92 bytes; the slack covers the MON-VER reply used as a
   * link probe, whose extension strings make it the longest thing the
   * receiver sends unsolicited-adjacent.
   */
  static constexpr uint16_t max_payload = 256U;

  /*
   * last_error() codes. The receiver has no error register to report, so
   * these are all synthetic and describe how far init() got.
   */
  static constexpr uint32_t err_none = 0U;
  static constexpr uint32_t err_no_link = 1U;   /**< No frame at either rate.*/
  static constexpr uint32_t err_cfg_rate = 2U;  /**< CFG-RATE not acked.     */
  static constexpr uint32_t err_cfg_msg = 3U;   /**< CFG-MSG not acked.      */

  /**
   * @brief   How the receiver is wired up, taken from the board header.
   */
  struct Config {
    SerialDriver *sd;        /**< Serial driver behind the GNSS pins.        */
    uint32_t baud_boot;      /**< Rate the receiver powers up at.            */
    uint32_t baud_run;       /**< Rate to switch the link to.                */
    uint16_t nav_period_ms;  /**< Navigation solution period, milliseconds.  */
  };

  /**
   * @brief   Link health counters.
   * @details These exist to tell a receiver that is alive but has no sky view
   *          from one that is not talking at all: both report fix_type_none,
   *          and without counters the two look identical from the log. A
   *          silently dead transport has cost this project a bring-up before,
   *          so the driver is built to be able to prove bytes are moving.
   */
  struct Stats {
    uint32_t bytes_rx;        /**< Bytes taken off the serial driver.        */
    uint32_t frames_ok;       /**< Frames that passed the checksum.          */
    uint32_t checksum_errors; /**< Frames whose checksum did not match.      */
    uint32_t resyncs;         /**< Parser restarts: bad sync or long length. */
    uint32_t naks;            /**< UBX-ACK-NAK replies to our configuration. */
    uint32_t timeouts;        /**< poll() calls that saw no byte at all.     */
  };

  explicit Ublox(const Config &cfg);

  /**
   * @brief   Starts the link and configures the receiver.
   * @details The bring-up order matters and is easy to get wrong:
   *
   *          1. Open at baud_run and poll UBX-MON-VER. A receiver still
   *             configured from an earlier run answers here and steps 2-3
   *             are skipped, which is the common case on a warm reset.
   *          2. Otherwise reopen at baud_boot and send UBX-CFG-PRT setting
   *             the new rate and UBX-only protocol masks. This frame is
   *             deliberately *not* acknowledged: the receiver changes rate as
   *             soon as it has parsed it, so any ACK would come back at the
   *             new speed. Waiting for one here always fails.
   *          3. Reopen at baud_run and confirm with MON-VER.
   *          4. CFG-RATE for the navigation period, then CFG-MSG to enable
   *             UBX-NAV-PVT. Both are acknowledged normally.
   *
   * @return  false if the link never came up or a setting was refused, see
   *          last_error().
   */
  bool init(void);

  /**
   * @brief   Reads and parses whatever the receiver has sent.
   * @param   timeout  how long to wait for the first byte.
   * @return  true if a new navigation solution was decoded, so nav_pvt() has
   *          fresh data.
   */
  bool poll(sysinterval_t timeout);

  /**
   * @brief   The most recent navigation solution.
   * @details Returned by value, copied under a lock, so a caller in another
   *          thread cannot observe a half-updated solution.
   */
  ubx_nav_pvt nav_pvt(void);

  /**
   * @brief   Link counters, see the Stats comment for why they are here.
   * @note    Cleared by init() once the link is confirmed, so they count the
   *          running link and not the noise from probing for it.
   */
  Stats stats(void) const { return stats_; }

  /** @brief  True once init() has brought the link up. */
  bool ready(void) const { return ready_; }

  /** @brief  How far init() got, one of the err_* codes. */
  uint32_t last_error(void) const { return error_; }

private:

  void open(uint32_t baud);
  bool probe(sysinterval_t timeout);
  msg_t get_until(systime_t deadline);
  void send(uint8_t cls, uint8_t id, const uint8_t *payload, uint16_t len);
  bool await(uint8_t cls, uint8_t id, sysinterval_t timeout);
  bool ack_wait(uint8_t cls, uint8_t id, sysinterval_t timeout);
  bool feed(uint8_t b);
  void dispatch(void);
  void decode_nav_pvt(void);

  Config cfg_;
  SerialConfig serial_cfg_ = {}; /**< Kept by reference inside sdStart().   */
  mutex_t lock_;                /**< Guards nav_pvt_ from the reader thread.*/
  ubx_nav_pvt nav_pvt_ = {};
  Stats stats_ = {};
  uint32_t error_ = err_none;
  bool ready_ = false;

  /*
   * Frame parser state. A whole frame is assembled here before anything looks
   * at it, so a truncated or corrupt one can never reach decode_nav_pvt().
   */
  uint8_t state_ = 0U;
  uint8_t msg_cls_ = 0U;
  uint8_t msg_id_ = 0U;
  uint16_t msg_len_ = 0U;
  uint16_t msg_pos_ = 0U;
  uint8_t ck_a_ = 0U;
  uint8_t ck_b_ = 0U;
  uint8_t payload_[max_payload] = {};

  /* Set by dispatch() for the frame it has just accepted. */
  bool have_nav_pvt_ = false;
  uint8_t ack_cls_ = 0U;
  uint8_t ack_id_ = 0U;
  bool ack_ok_ = false;
  bool ack_seen_ = false;
};

} /* namespace pika::gnss */

