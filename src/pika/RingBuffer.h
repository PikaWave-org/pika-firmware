#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <iterator>
#include <span>

namespace pika {

// Ring buffer of SIZE elements, not thread safe: use it from one context, or
// ConcurrentRingBuffer between a writer and a reader in different contexts.
// COUNT is the element count's type, which is all the two differ in.
template<class T, size_t SIZE, class COUNT = size_t>
class RingBuffer {
    static_assert(SIZE > 0U, "SIZE must not be 0");

public:
    size_t size() const { return load(size_); }

    size_t free() const { return SIZE - size(); }

    bool empty() const { return size() == 0U; }

    bool full() const { return size() >= SIZE; }

    // Writer: up to n free elements at the write position, contiguous, so
    // fewer if the ring wraps first. Filled in place, then published by
    // commit(); call again for the rest.
    std::span<T> write_span(size_t n) { return {&data_[w_], std::min({n, free(), SIZE - w_})}; }

    void commit(size_t n) {
        advance(w_, n);
        add(size_, n);
    }

    // Reader: up to n queued elements at the read position, contiguous, so
    // fewer if the ring wraps first. Valid until pop(); call again for the
    // rest.
    std::span<T> read_span(size_t n) { return {&data_[r_], std::min({n, size(), SIZE - r_})}; }

    T &get(size_t idx) {
        size_t i = r_ + idx;
        if (i >= SIZE) {
            i -= SIZE;
        }
        return data_[i];
    }

    // Reader: the queued elements, oldest first. begin() takes the count, so
    // elements the writer adds meanwhile are not visited.
    template<class V>
    class Iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = V *;
        using reference = V &;

        Iterator() = default;

        Iterator(V *data, size_t pos, size_t left) : data_(data), pos_(pos), left_(left) {}

        reference operator*() const { return data_[pos_]; }

        pointer operator->() const { return &data_[pos_]; }

        Iterator &operator++() {
            advance(pos_, 1U);
            left_--;
            return *this;
        }

        Iterator operator++(int) {
            Iterator prev = *this;
            ++*this;
            return prev;
        }

        bool operator==(const Iterator &other) const { return left_ == other.left_; }

    private:
        V *data_{};
        size_t pos_{};
        size_t left_{};
    };

    using iterator = Iterator<T>;
    using const_iterator = Iterator<const T>;

    iterator begin() { return {data_, r_, size()}; }

    iterator end() { return {data_, r_, 0U}; }

    const_iterator begin() const { return {data_, r_, size()}; }

    const_iterator end() const { return {data_, r_, 0U}; }

    void pop(size_t n) {
        advance(r_, n);
        sub(size_, n);
    }

    // Empties the ring; only while neither side runs.
    void reset() {
        w_ = 0U;
        r_ = 0U;
        size_ = 0U;
    }

private:
    static size_t load(const size_t &count) { return count; }

    static void add(size_t &count, size_t n) { count += n; }

    static void sub(size_t &count, size_t n) { count -= n; }

    // ConcurrentRingBuffer: the release/acquire on the count orders the
    // elements written before it against the ones read after it.
    static size_t load(const std::atomic<size_t> &count) { return count.load(std::memory_order_acquire); }

    static void add(std::atomic<size_t> &count, size_t n) { count.fetch_add(n, std::memory_order_release); }

    static void sub(std::atomic<size_t> &count, size_t n) { count.fetch_sub(n, std::memory_order_release); }

    static void advance(size_t &i, size_t n) {
        i += n;
        if (i >= SIZE) {
            i -= SIZE;
        }
    }

    T data_[SIZE]{};
    size_t w_{};// Writer index
    size_t r_{};// Reader index
    COUNT size_{};
};

// Single producer, single consumer ring, lock free: one context writes
// (write_span, commit), another reads (read_span, get, iterators, pop), each a
// thread or an interrupt. Each side owns its index; the shared count is atomic.
template<class T, size_t SIZE>
class ConcurrentRingBuffer : public RingBuffer<T, SIZE, std::atomic<size_t>> {
    static_assert(std::atomic<size_t>::is_always_lock_free, "the count must be lock free");
};

}// namespace pika
