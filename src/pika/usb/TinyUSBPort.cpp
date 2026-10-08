#include "TinyUSBPort.h"

namespace {
static event_source_t g_tinyusb_event_source;
static bool g_tinyusb_event_inited = false;

static void tinyusb_event_init() {
    if (!g_tinyusb_event_inited) {
        osalEventObjectInit(&g_tinyusb_event_source);
        g_tinyusb_event_inited = true;
    }
}
} // namespace

void tud_cdc_rx_cb(uint8_t itf) {
    (void) itf;
    if (!g_tinyusb_event_inited) {
        return;
    }

    osalEventBroadcastFlags(&g_tinyusb_event_source, CHN_INPUT_AVAILABLE);
}

void tud_cdc_tx_complete_cb(uint8_t itf) {
    (void) itf;
    if (!g_tinyusb_event_inited) {
        return;
    }

    osalEventBroadcastFlags(&g_tinyusb_event_source, CHN_OUTPUT_EMPTY);
}

namespace pika::usb {
TinyUSBPort::TinyUSBPort() {
    tinyusb_event_init();
}

ssize_t TinyUSBPort::write(const uint8_t *buf, size_t n) {
    uint32_t available = tud_cdc_write_available();

    // max packet size is 64 bytes or write available
    available = available > 64 ? 64 : available;
    n = n < available ? n : available;

    uint32_t count = tud_cdc_write(buf, n);
    tud_cdc_write_flush();
    return count;
}

ssize_t TinyUSBPort::read(uint8_t *buf, size_t n) {
    uint32_t available = tud_cdc_available();
    if (available == 0) {
        return 0;
    }

    uint32_t count = tud_cdc_read(buf, n);
    return count;
}

void TinyUSBPort::close() {
}

bool TinyUSBPort::is_open() {
    return tud_cdc_connected();
}

const event_source_t *TinyUSBPort::event_source() const {
    return &g_tinyusb_event_source;
}

event_source_t *TinyUSBPort::event_source() {
    return &g_tinyusb_event_source;
}

const char *TinyUSBPort::get_driver_name() const {
    return "TinyUSB";
}
} // namespace pika::drivers
