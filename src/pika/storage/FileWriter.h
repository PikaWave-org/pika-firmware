#pragma once

#include <pika/RingBuffer.h>

#include <ch.hpp>

namespace pika {

class FileWriter : public chibios_rt::BaseStaticThread<1024> {
public:
    FileWriter() = default;

    void main() override;

    bool open(const char *fn);

    bool close();

    void write(std::span<const uint8_t> buf);
private:

};

}// namespace pika
