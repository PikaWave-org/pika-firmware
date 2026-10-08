#pragma once

#include "board.h"

#include <cstdint>

namespace pika::version {

constexpr char HARDWARE[] = BOARD_NAME;

constexpr uint16_t USB_VERSION = 0x0200;
constexpr uint16_t USB_VENDOR_ID = 0x1209;
constexpr uint16_t USB_PRODUCT_ID = 0x0000;
constexpr char USB_VENDOR_STR[] = "Pikawave";
const constexpr char * const USB_PRODUCT_STR = HARDWARE;
constexpr char USB_PRODUCT_REV[] = "0";
constexpr char USB_SERIAL_NUMBER[] = "123456789ABC";

}
