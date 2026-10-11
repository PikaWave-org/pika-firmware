#pragma once

#include <ch.hpp>
#include <hal.h>

#include "tusb.h"

namespace pika::usb {

class TinyUSBTask : public chibios_rt::BaseStaticThread<2048> {
protected:
    void main() override {
        setName("tinyUSB");

        const tusb_rhport_init_t init = {.role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_HIGH};
        LOG("tinyUSB init");
        tusb_init(BOARD_TUD_RHPORT, &init);

        while (not shouldTerminate()) {
            // tinyusb device task
            tud_task();

            if (tud_suspended()) {
                chThdSleepSeconds(2);
                LOG("tinyUSB reinit");
                tud_deinit(BOARD_TUD_RHPORT);
                tusb_init(BOARD_TUD_RHPORT, &init);
            }
        }

        tud_deinit(BOARD_TUD_RHPORT);
    }
};

}// namespace pika::usb
