#pragma once
#include "MAC.h"

namespace pika {
class Dispatcher {
public:
    Dispatcher(MAC &mac) : mac(mac) {}

private:
    MAC *mac_{};
};
}// namespace pika