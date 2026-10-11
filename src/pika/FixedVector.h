#pragma once

#include "ch.h"

#include <array>
#include <cstddef>
#include <span>

namespace pika {

// A vector with fixed capacity and no allocation.
template<class T, size_t SIZE>
class FixedVector {
public:
    T *data() { return storage_.data(); }

    const T *data() const { return storage_.data(); }

    size_t size() const { return size_; }

    // Unchecked, like std::vector.
    T &operator[](size_t idx) { return storage_[idx]; }

    const T &operator[](size_t idx) const { return storage_[idx]; }

    // Checked against size(); out of range halts, as there are no exceptions.
    T &at(size_t idx) {
        if (idx >= size_) {
            chSysHalt("FixedVector::at: out of range");
        }
        return storage_[idx];
    }

    const T &at(size_t idx) const {
        if (idx >= size_) {
            chSysHalt("FixedVector::at: out of range");
        }
        return storage_[idx];
    }

    T *add(const T &value) {
        if (size_ < storage_.size()) {
            T *ret = &storage_[size_];
            *ret = value;
            ++size_;
            return ret;
        }
        return nullptr;
    }

    T *add() {
        if (size_ < storage_.size()) {
            T *ret = &storage_[size_];
            ++size_;
            return ret;
        }
        return nullptr;
    }

    void remove_last() {
        if (size_ > 0) {
            --size_;
        }
    }

    void resize(size_t sz) { size_ = sz; }

    void clear() { size_ = 0; }

    size_t cap() { return storage_.size(); }

    operator std::span<T>() { return {storage_.data(), size_}; }

    T *begin() { return storage_.data(); }

    T *end() { return storage_.data() + size_; }

    bool full() const { return size_ >= storage_.size(); }

    bool empty() const { return size_ == 0; }

private:
    std::array<T, SIZE> storage_{};
    size_t size_{};
};

}// namespace pika
