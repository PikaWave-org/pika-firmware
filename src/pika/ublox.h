/**
 * @file ublox.h
 * Driver for a u-blox GNSS receiver connected via serial port.
 *
 * Adapted from the microavia UBX driver. The framing, the packed message
 * structs and the configuration handshake are that driver's; what changed is
 * the platform underneath it - ChibiOS instead of the microavia Task and
 * SerialPort, std::span instead of Slice - and the output, which stays in
 * memory behind the getters below instead of being published to a topic.
 *
 * Nothing here is specific to one receiver: UBX framing, the CFG messages and
 * NAV-PVT are common across the u-blox generations. The module on pika_go_0
 * is a CAM-M8Q-0, an M8 part, so NAV-PVT is what gets enabled.
 *
 * Usage:
 *
 *   static const pika::gnss::Ublox::Config gnss_cfg = {
 *     &BOARD_GNSS_SERIAL, BOARD_GNSS_BAUD_RUN, 1000U, 1
 *   };
 *   static pika::gnss::Ublox gnss{gnss_cfg};
 *
 *   chThdCreateStatic(waGnss, sizeof(waGnss), NORMALPRIO, gnss_thread, &gnss);
 *   // ... in that thread: gnss.run();
 *
 *   pika::gnss::msg_rx_nav_pvt_s pvt;
 *   if (gnss.nav_pvt(pvt)) { ... }
 */

#pragma once

#include <cstdint>
#include <span>

#include "ch.h"
#include "hal.h"

namespace pika::gnss {

#pragma pack(push, 1)

/**
 * @brief   RX NAV-PVT, the navigation solution: position, velocity and time.
 * @note    Units are u-blox's own. This is the wire layout, cast in place out
 *          of the receive buffer, so the field order is the message's and the
 *          reserved bytes are present.
 */
struct msg_rx_nav_pvt_s {
    uint32_t iTOW;      ///< GPS Time of Week [ms]
    uint16_t year;      ///< Year, range 1999..2099 (UTC)
    uint8_t month;      ///< Month, range 1..12 (UTC)
    uint8_t day;        ///< Day of month, range 1..31 (UTC)
    uint8_t hour;       ///< Hour of day, range 0..23 (UTC)
    uint8_t min;        ///< Minute of hour, range 0..59 (UTC)
    uint8_t sec;        ///< Seconds of minute, range 0..60 (UTC)
    uint8_t valid;      ///< Validity Flags (bit 0: validDate, bit 1: validTime, bit 2: fullyResolved)
    uint32_t tAcc;      ///< Time accuracy estimate (UTC) [ns]
    int32_t nano;       ///< Fraction of second, range -1e9 .. 1e9 (UTC) [ns]
    uint8_t fixType;    ///< GNSS fix type (0: no fix, 1: dead reckoning only, 2: 2D-fix, 3: 3D-fix, 4: GNSS + dead reckoning combined, 5: time only fix)
    uint8_t flags;      ///< Fix status flags (bit 0: gnssFixOK, bit 1: diffSoln, bit 4: psmState, bit 5: headVehValid, bit 6: RTKfloat)
    uint8_t flags2;     ///< Additional flags (bit 5: confirmedAvai, bit 6: confirmedDate, bit 7: confirmedTime)
    uint8_t numSV;      ///< Number of satellites used in Nav Solution
    int32_t lon;        ///< Longitude [1e-7 deg]
    int32_t lat;        ///< Latitude [1e-7 deg]
    int32_t height;     ///< Height above ellipsoid [mm]
    int32_t hMSL;       ///< Height above mean sea level [mm]
    uint32_t hAcc;      ///< Horizontal accuracy estimate [mm]
    uint32_t vAcc;      ///< Vertical accuracy estimate [mm]
    int32_t velN;       ///< North velocity component [mm/s]
    int32_t velE;       ///< East velocity component [mm/s]
    int32_t velD;       ///< Down velocity component [mm/s]
    uint32_t gSpeed;    ///< Ground speed (2-D) [mm/s]
    int32_t headMot;    ///< Heading of motion 2-D [1e-5 deg]
    uint32_t sAcc;      ///< Speed accuracy estimate [mm/s]
    uint32_t headAcc;   ///< Heading accuracy estimate (both motion and vehicle) [1e-5 deg]
    uint16_t pDOP;      ///< Position DOP [0.01]
    uint8_t reserved1[6];///< Reserved
    int32_t headVeh;    ///< Heading of vehicle (2-D) [1e-5 deg]
    uint8_t reserved2[4];///< Reserved
};

/**
 * @brief   RX MON-HW, the state of the receiver's front end.
 * @note    The M8 layout, 60 bytes. Only the noise, AGC, antenna and jamming
 *          fields are of interest here; the pin maps are carried because this
 *          is the wire layout, cast in place out of the receive buffer.
 * @note    jamInd and the jammingState bits only mean anything once the
 *          interference monitor has been switched on with CFG-ITFM, which the
 *          driver does not do. Until then they read 0 and "unknown".
 */
struct msg_rx_mon_hw_s {
    uint32_t pinSel;    ///< Mask of pins set as peripheral/PIO
    uint32_t pinBank;   ///< Mask of pins set as bank A/B
    uint32_t pinDir;    ///< Mask of pins set as input/output
    uint32_t pinVal;    ///< Mask of pins value low/high
    uint16_t noisePerMS;///< Noise level as measured by the GPS core
    uint16_t agcCnt;    ///< AGC monitor, range 0..8191
    uint8_t aStatus;    ///< Antenna supervisor state (0: INIT, 1: DONTKNOW, 2: OK, 3: SHORT, 4: OPEN)
    uint8_t aPower;     ///< Antenna power status (0: off, 1: on, 2: don't know)
    uint8_t flags;      ///< Flags (bit 0: rtcCalib, bit 1: safeBoot, bits 2..3: jammingState, bit 4: xtalAbsent)
    uint8_t reserved1;  ///< Reserved
    uint32_t usedMask;  ///< Mask of pins that are used by the virtual pin manager
    uint8_t VP[17];     ///< Array of pin mappings for each of the 17 physical pins
    uint8_t jamInd;     ///< CW jamming indicator, range 0 (none) .. 255 (strong)
    uint8_t reserved2[2];///< Reserved
    uint32_t pinIrq;    ///< Mask of pins value using the PIO Irq
    uint32_t pullH;     ///< Mask of pins value using the PIO pull high resistor
    uint32_t pullL;     ///< Mask of pins value using the PIO pull low resistor
};

#pragma pack(pop)

static_assert(sizeof(msg_rx_nav_pvt_s) == 92);
static_assert(sizeof(msg_rx_mon_hw_s) == 60);

/// Antenna supervisor states, the values of msg_rx_mon_hw_s::aStatus.
enum class AntennaStatus : uint8_t {
    INIT = 0,
    DONTKNOW = 1,
    OK = 2,
    SHORT = 3,
    OPEN = 4
};

/// Jamming/interference monitor states, bits 2..3 of msg_rx_mon_hw_s::flags.
enum class JammingState : uint8_t {
    UNKNOWN = 0,    ///< Monitor disabled - what this driver leaves it at
    OK = 1,         ///< No significant jamming
    WARNING = 2,    ///< Interference visible, fix still held
    CRITICAL = 3    ///< Interference visible, no fix
};

/// Pulls the jammingState field out of msg_rx_mon_hw_s::flags.
inline JammingState jamming_state(const msg_rx_mon_hw_s &msg) {
    return static_cast<JammingState>((msg.flags >> 2) & 0x03U);
}

class Ublox {
public:
    /**
     * @brief   How the receiver is wired up, taken from the board header.
     */
    struct Config {
        SerialDriver *port;     ///< Serial driver the receiver is on
        uint32_t baudrate;      ///< Baudrate to run the link at
        uint16_t meas_int_ms;   ///< Measurement interval [ms]
        uint8_t port_id;        ///< Receiver's own port number, 1 for UART1
    };

    explicit Ublox(const Config &cfg);

    /// Configure the receiver and read from it forever. Give it a thread.
    void run();

    /// Last NAV-PVT received.
    /**
     * Copied out under the lock, so the caller cannot observe a half-updated
     * message while the driver thread is writing one.
     *
     * @return false if no NAV-PVT has arrived yet, leaving msg untouched.
     */
    bool nav_pvt(msg_rx_nav_pvt_s &msg) const;

    /// Last MON-HW received: noise, AGC, antenna and jamming state.
    /**
     * Copied out under the same lock as nav_pvt(), for the same reason.
     *
     * @return false if no MON-HW has arrived yet, leaving msg untouched.
     */
    bool mon_hw(msg_rx_mon_hw_s &msg) const;

    /// Frames received, and frames dropped for a bad sync or checksum.
    /**
     * A receiver with no sky view and a receiver that is not talking at all
     * both report fixType 0, so the counters are what tells them apart.
     */
    uint32_t frames() const { return _frames; }

    uint32_t errors() const { return _errors; }

private:
    static constexpr size_t BUFFER_SIZE = 512;
    static constexpr size_t MAX_TX_PAYLOAD = 32;
    static constexpr sysinterval_t RECV_TIMEOUT = TIME_MS2I(5);
    static constexpr sysinterval_t RECV_VER_TIMEOUT = TIME_MS2I(100);
    static constexpr sysinterval_t RECV_ACK_TIMEOUT = TIME_MS2I(200);

#pragma pack(push, 1)

// UBX Message Header
    struct Message {
        uint8_t sync1;
        uint8_t sync2;
        uint8_t msg_class;
        uint8_t msg_id;
        uint16_t length;

        void *payload() {
            return reinterpret_cast<uint8_t *>(this) + sizeof(Message);
        }
    };

// UBX Message Checksum
    struct Checksum {
        uint8_t ck_a;
        uint8_t ck_b;
    };
#pragma pack(pop)

    enum class HWVersion {
        NONE,
        UNKNOWN,
        UBLOX8,
        UBLOX9
    };

    struct HWProtocolVersion {
        HWVersion hwVersion = HWVersion::NONE;
        int protocol = 0;

        bool found() const {
            return hwVersion != HWVersion::NONE;
        }
    };

    // Message Classes
    enum class MessageClass : uint8_t {
        NAV = 0x01,
        ACK = 0x05,
        CFG = 0x06,
        MON = 0x0A
    };

    // Message IDs
    enum class MessageID : int {
        ANY = -1,
        NAV_PVT = 0x07,

        ACK_NAK = 0x00,
        ACK_ACK = 0x01,

        CFG_PRT = 0x00,
        CFG_MSG = 0x01,
        CFG_RATE = 0x08,
        CFG_NAV5 = 0x24,

        MON_VER = 0x04,
        MON_HW = 0x09,
    };

    Config _cfg;

    mutable mutex_t _lock;      ///< Guards _nav_pvt and _mon_hw against the getters
    msg_rx_nav_pvt_s _nav_pvt = {};
    bool _got_nav_pvt = false;
    msg_rx_mon_hw_s _mon_hw = {};
    bool _got_mon_hw = false;

    HWProtocolVersion _version;

    /*
     * Receive buffer and the window of it still to be parsed. Messages are
     * cast in place out of this, which is why the structs above are packed:
     * a frame can start at any offset, so nothing may assume alignment.
     */
    uint8_t _buffer[BUFFER_SIZE] = {};
    size_t _fill = 0;           ///< Bytes held in _buffer
    size_t _pos = 0;            ///< Parse cursor into _buffer

    bool _sync = false;

    uint32_t _frames = 0;
    uint32_t _errors = 0;

    static const uint32_t _baudrates_list[8];

    std::span<uint8_t> parse_buf() {
        return {&_buffer[_pos], _fill - _pos};
    }

    void set_baudrate(uint32_t baudrate);

    void flush_buffers();

    HWProtocolVersion probe();

    /// Read message.
    /**
     * Read one message from the receiver's serial port. Returns immediately
     * if a complete message is already buffered, otherwise waits up to
     * RECV_TIMEOUT for more data.
     *
     * @return true if a message was read, false if not enough data collected.
     */
    bool read_message(Message *&msg);

    bool wait_for_message(sysinterval_t timeout, Message *&msg, MessageClass msg_class, MessageID msg_id);

    bool send_message(MessageClass msg_class, MessageID msg_id, const void *payload, size_t payload_length,
                      bool wait_ack = false);

    void send_cfg_msg(MessageClass msg_class, MessageID msg_id, uint8_t rate, bool wait_ack);

    void send_data(const uint8_t *data, size_t length);

    void configure();

    void handle_message(Message *msg);

    static const char *hw_version_string(HWVersion version);

    static Checksum calculate_checksum(std::span<const uint8_t> data);
};

} /* namespace pika::gnss */
