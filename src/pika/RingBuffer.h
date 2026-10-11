#pragma once

#include <algorithm>
#include <atomic>
#include <span>

namespace pika {

// Single producer, single consumer ring, lock free: one context writes, one
// reads, each a thread or an interrupt. Each side owns its index; the shared
// element count is atomic, and its release/acquire orders the elements.
template<class T, size_t SIZE>
class RingBuffer {
    static_assert(SIZE > 0U, "SIZE must not be 0");
    static_assert(std::atomic<size_t>::is_always_lock_free, "the count must be lock free");

public:
    size_t size() const { return size_.load(std::memory_order_acquire); }

    size_t free() const { return SIZE - size(); }

    bool empty() const { return size() == 0U; }

    bool full() const { return size() == SIZE; }

    // Writer: up to n free elements at the write position, contiguous, so
    // fewer if the ring wraps first. Filled in place, then published by
    // commit(); call again for the rest.
    std::span<T> write_span(size_t n) { return {&data_[w_], std::min({n, free(), SIZE - w_})}; }

    void commit(size_t n) {
        advance(w_, n);
        size_.fetch_add(n, std::memory_order_release);
    }

    // Reader: up to n queued elements at the read position, contiguous, so
    // fewer if the ring wraps first. Valid until pop(); call again for the
    // rest.
    std::span<T> read_span(size_t n) { return {&data_[r_], std::min({n, size(), SIZE - r_})}; }

    void pop(size_t n) {
        advance(r_, n);
        size_.fetch_sub(n, std::memory_order_release);
    }

    // Empties the ring; only while neither side runs.
    void reset() {
        w_ = 0U;
        r_ = 0U;
        size_.store(0U, std::memory_order_relaxed);
    }

private:
    static void advance(size_t &i, size_t n) {
        i += n;
        if (i >= SIZE) {
            i -= SIZE;
        }
    }

    T data_[SIZE]{};
    size_t w_{};// Writer index
    size_t r_{};// Reader index
    std::atomic<size_t> size_{};
};

}// namespace pika
