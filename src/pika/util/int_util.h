#pragma once

#include <bit>
#include <cstdint>
#include <type_traits>

namespace pika::util {

template <class T>
constexpr T to_big_endian(T v) {
    static_assert(std::is_integral_v<T>);
    if constexpr (std::endian::native == std::endian::big) {
        return v;
    } else {
        return std::byteswap(v);
    }
}

template <class T>
constexpr T to_little_endian(T v) {
    static_assert(std::is_integral_v<T>);
    if constexpr (std::endian::native == std::endian::little) {
        return v;
    } else {
        return std::byteswap(v);
    }
}

template <class T>
constexpr T from_big_endian(T v) {
    return to_big_endian(v);
}

template <class T>
constexpr T from_little_endian(T v) {
    return to_little_endian(v);
}

} // namespace microavia