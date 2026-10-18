#pragma once

namespace pika::radio {

struct RadioStats {
    float snr_packet;
    float rssi_packet;
    float rssi_floor;
};

}
