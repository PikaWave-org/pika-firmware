#pragma once

#include <bit>
#include <concepts>
#include <cstdint>
#include <cstring>

namespace pika::util {

template<std::integral T>
constexpr T to_big_endian(T v) {
    if constexpr (std::endian::native == std::endian::big) {
        return v;
    } else {
        return std::byteswap(v);
    }
}

template<std::integral T>
constexpr T to_little_endian(T v) {
    if constexpr (std::endian::native == std::endian::little) {
        return v;
    } else {
        return std::byteswap(v);
    }
}

template<std::integral T>
constexpr T from_big_endian(T v) {
    return to_big_endian(v);
}

template<std::integral T>
constexpr T from_little_endian(T v) {
    return to_little_endian(v);
}

// Stores v at dst in the given byte order; dst needs no alignment.
template<std::integral T>
void to_big_endian(uint8_t *dst, T v) {
    v = to_big_endian(v);
    std::memcpy(dst, &v, sizeof(v));
}

template<std::integral T>
void to_little_endian(uint8_t *dst, T v) {
    v = to_little_endian(v);
    std::memcpy(dst, &v, sizeof(v));
}

// Reads a T stored at src in the given byte order; src needs no alignment.
template<std::integral T>
T from_big_endian(const uint8_t *src) {
    T v;
    std::memcpy(&v, src, sizeof(v));
    return from_big_endian(v);
}

template<std::integral T>
T from_little_endian(const uint8_t *src) {
    T v;
    std::memcpy(&v, src, sizeof(v));
    return from_little_endian(v);
}

}// namespace pika::util
