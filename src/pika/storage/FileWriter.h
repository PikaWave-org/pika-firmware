#pragma once

#include <pika/RingBuffer.h>

#include <ch.hpp>

#include <pika/fatfs_wrapper/fatfsWrapper.h>

#include "ff.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <sys/types.h>

namespace pika {

// Writes a file from its own thread through a ring buffer, so callers never
// block on the card. All FatFS calls for the file happen on that thread.
template<size_t BUF_SIZE>
class FileWriter : public chibios_rt::BaseStaticThread<1024> {
public:
    FileWriter() = default;

    chibios_rt::ThreadReference start(tprio_t prio) override {
        thread_ = BaseStaticThread::start(prio);
        return thread_;
    }

    void main() override {
        setName("file writer");

        while (true) {
            chEvtWaitAny(EVT_WAKE);

            if (req_open_) {
                result_ = wf_open(&file_, name_, FA_WRITE | FA_CREATE_ALWAYS);
                open_ = result_ == FR_OK;
                req_open_ = false;
            }

            while (not buf_.empty()) {
                const std::span<uint8_t> data = buf_.read_span(buf_.size());
                if (open_ and result_ == FR_OK) {
                    UINT written = 0;
                    const FRESULT res = wf_write(&file_, data.data(), data.size(), &written);
                    if (res != FR_OK or written != data.size()) {
                        result_ = res != FR_OK ? res : FR_DENIED;// card full
                    }
                }
                buf_.pop(data.size());
            }

            if (req_close_) {
                const FRESULT res = wf_close(&file_);
                if (result_ == FR_OK) {
                    result_ = res;
                }
                open_ = false;
                req_close_ = false;
            }

            chSysLock();
            if (idle()) {
                chThdResumeS(&waiter_, MSG_OK);
            }
            chSysUnlock();
        }
    }

    /**
     * Open file, blocking.
     * @param fn file name
     * @return true on success, false on error
     */
    bool open(const char *fn) {
        if (open_ or not idle()) {
            return false;
        }
        const size_t len = strnlen(fn, sizeof(name_) - 1U);
        std::copy_n(fn, len, name_);
        name_[len] = '\0';
        result_ = FR_OK;
        req_open_ = true;
        thread_.signalEvents(EVT_WAKE);
        wait(TIME_INFINITE);
        return open_;
    }

    /**
     * Close file, non blocking. Asynchronously flushes buffer and closes the file after.
     * @return true on success, false on error
     */
    bool close() {
        if (not open_ or req_close_) {
            return false;
        }
        req_close_ = true;
        thread_.signalEvents(EVT_WAKE);
        return result_ == FR_OK;
    }

    /**
     * Write to file, non blocking. Actual write will happen asynchronously from write own thread.
     * @param buf buffer to write
     * @return number of bytes written (may be smaller than requested if buffer is full) or < 0 in case of error
     */
    ssize_t write(std::span<const uint8_t> buf) {
        if (not open_ or req_close_ or result_ != FR_OK) {
            return -1;
        }

        // Split in two if the ring wraps.
        const std::span<uint8_t> first = buf_.write_span(buf.size());
        std::copy_n(buf.data(), first.size(), first.data());
        buf_.commit(first.size());
        size_t n = first.size();

        if (n < buf.size()) {
            const std::span<uint8_t> rest = buf_.write_span(buf.size() - n);
            std::copy_n(buf.data() + n, rest.size(), rest.data());
            buf_.commit(rest.size());
            n += rest.size();
        }

        thread_.signalEvents(EVT_WAKE);
        return (ssize_t) n;
    }

    /**
     * Wait until the last operation (write or close) will complete.
     * @param timeout waiting timeout
     * @return true if finished, false on tieout
     */
    bool wait(sysinterval_t timeout) {
        msg_t msg = MSG_OK;
        chSysLock();
        if (not idle()) {
            msg = chThdSuspendTimeoutS(&waiter_, timeout);
        }
        chSysUnlock();
        return msg == MSG_OK;
    }

    /** First FatFS error since open(), FR_OK if none. */
    FRESULT result() const { return result_; }

    /** Bytes write() accepts right now without coming up short. */
    size_t free() const { return buf_.free(); }

private:
    static constexpr eventmask_t EVT_WAKE = EVENT_MASK(0);

    bool idle() const { return not req_open_ and not req_close_ and buf_.empty(); }

    ConcurrentRingBuffer<uint8_t, BUF_SIZE> buf_;
    chibios_rt::ThreadReference thread_;
    thread_reference_t waiter_ = nullptr;

    FIL file_{};
    char name_[64]{};
    volatile bool open_ = false;
    volatile bool req_open_ = false;
    volatile bool req_close_ = false;
    volatile FRESULT result_ = FR_OK;
};

}// namespace pika
