#include "App.h"

#include <pika/fatfs_wrapper/fatfsWrapper.h>

#include "ff.h"

#include <charconv>
#include <cstring>
#include <string_view>

namespace pika::app {
namespace {

std::string_view trim(std::string_view s) {
    constexpr std::string_view space = " \t\r\n";
    const size_t first = s.find_first_not_of(space);
    if (first == std::string_view::npos) {
        return {};
    }
    return s.substr(first, s.find_last_not_of(space) - first + 1);
}

// "<id as hex>,<name>,<key as hex>"; the name may itself contain commas.
bool parse_peer(std::string_view line, Peer &peer) {
    const size_t first_comma = line.find(',');
    const size_t last_comma = line.rfind(',');
    if (first_comma == std::string_view::npos or first_comma == last_comma) {
        return false;
    }
    const std::string_view id = trim(line.substr(0, first_comma));
    const std::string_view name = trim(line.substr(first_comma + 1, last_comma - first_comma - 1));
    const std::string_view key = trim(line.substr(last_comma + 1));

    if (id.empty() or id.size() > 16U or name.empty() or name.size() >= peer.name.size() or
        key.size() != peer.key.size() * 2) {
        return false;
    }
    const auto [id_end, id_ec] = std::from_chars(id.data(), id.data() + id.size(), peer.addr, 16);
    if (id_ec != std::errc{} or id_end != id.data() + id.size()) {
        return false;
    }
    for (size_t i = 0; i < peer.key.size(); i++) {
        const char *byte = key.data() + 2 * i;
        const auto [end, ec] = std::from_chars(byte, byte + 2, peer.key[i], 16);
        if (ec != std::errc{} or end != byte + 2) {
            return false;
        }
    }
    *std::copy(name.begin(), name.end(), peer.name.begin()) = '\0';
    return true;
}

// Lower case hex, zero padded to digits; returns the end.
char *put_hex(char *out, uint64_t value, int digits) {
    for (int i = digits - 1; i >= 0; i--) {
        out[i] = "0123456789abcdef"[value & 0xFU];
        value >>= 4U;
    }
    return out + digits;
}

// One past the highest hex-named file in dir, so ids survive a reboot.
uint32_t next_msg_id(const char *dir) {
    static DIR d;
    static FILINFO info;
    uint32_t last = 0;
    if (wf_opendir(&d, dir) == FR_OK) {
        while (wf_readdir(&d, &info) == FR_OK and info.fname[0] != '\0') {
            uint32_t id = 0;
            const char *end = info.fname + strlen(info.fname);
            const auto [ptr, ec] = std::from_chars(info.fname, end, id, 16);
            if (ec == std::errc{} and ptr == end and id > last) {
                last = id;
            }
        }
        wf_closedir(&d);
    }
    return last + 1U;
}

}// namespace

void App::load_peers() {
    // Static: too big for a thread stack.
    static FIL file;
    static char line[200];

    peers_.clear();
    FRESULT res = wf_open(&file, "/contacts.txt", FA_READ);
    if (res != FR_OK) {
        LOG("peers: cannot open contacts.txt, error %d", (int) res);
        return;
    }
    unsigned line_no = 0;
    while (wf_gets(line, sizeof(line), &file) != nullptr) {
        line_no++;
        size_t len = strlen(line);
        if (line[len - 1] != '\n' and not wf_eof(&file)) {
            LOG("peers: line %u too long, ignored", line_no);
            while (wf_gets(line, sizeof(line), &file) != nullptr and line[strlen(line) - 1] != '\n') {}
            continue;
        }
        const std::string_view text = trim(line);
        if (text.empty() or text.front() == '#') {
            continue;
        }
        if (peers_.full()) {
            LOG("peers: more than %u, rest ignored", (unsigned) peers_.size());
            break;
        }
        if (not parse_peer(text, *peers_.add())) {
            peers_.remove_last();
            LOG("peers: line %u malformed, ignored", line_no);
        }
    }
    if (wf_error(&file)) {
        LOG("peers: read error");
    }
    wf_close(&file);
    LOG("peers: %u loaded", (unsigned) peers_.size());
}

void App::main() {
    LOG("app main start");
    event_listener_t listener;
    chEvtRegister(&buttons_.events(), &listener, 0);

    while (true) {
        if (chEvtWaitAnyTimeout(EVENT_MASK(0), TIME_S2I(1))) {
            systime_t now = chVTGetSystemTime();
            const eventflags_t flags = chEvtGetAndClearFlags(&listener);
            for (unsigned i = 0; i < input::Buttons::count; i++) {
                const auto btn = (input::Button) i;
                const bool press = (flags & buttons_.press_flag(btn)) != 0U;
                const bool release = (flags & buttons_.release_flag(btn)) != 0U;

                if ((not press and not release) or active_screen_ == nullptr) {
                    continue;
                }
                backlight_wake_ = now;
                // A quick tap can bring both; the press comes first.
                if (press) {
                    LOG("press %s", input::Buttons::name(btn));
                    active_screen_->on_press(btn);
                }
                if (release) {
                    LOG("release %s", input::Buttons::name(btn));
                    active_screen_->on_release(btn);
                }
                cfg_.display->flush();
            }
        }
        if (active_screen_->show_status_bar()) {
            status_bar_.draw();
            cfg_.display->flush();
        }
        if (chVTTimeElapsedSinceX(backlight_wake_) >= backlight_timeout_) {
            do_set_backlight(0);
        } else {
            do_set_backlight(backlight_brightness_);
        }
    }
}

void App::init() {
    LOG("app init");
    msg_writer_.start(NORMALPRIO);
    load_peers();
    set_backlight(500);
    buttons_.init();
    active_screen_->init();
    activate_screen(home_screen_);
}

void App::set_backlight(int brightness) {
    LOG("Setting backlight to %d/%d", brightness, cfg_.backlight_pwm->config->period);
    backlight_brightness_ = brightness;
    do_set_backlight(backlight_brightness_);
}

bool App::start_recording(size_t peer_index) {
    if (recording_peer_ != nullptr or peer_index >= peers_.size() or not cfg_.recorder->idle()) {
        return false;
    }
    Peer &peer = peers_[peer_index];

    // "/<peer id>/tx/<message id>", creating the directories on the way.
    char *p = msg_path_;
    *p++ = '/';
    p = put_hex(p, peer.addr, 16);
    *p = '\0';
    wf_mkdir(msg_path_);
    p = std::copy_n("/tx", 3, p);
    *p = '\0';
    wf_mkdir(msg_path_);
    const uint32_t id = next_msg_id(msg_path_);
    *p++ = '/';
    p = put_hex(p, id, 8);
    *p = '\0';

    if (not msg_writer_.open(msg_path_)) {
        LOG("recording: cannot create %s, error %d", msg_path_, (int) msg_writer_.result());
        return false;
    }
    dropped_frames_ = 0;

    // Before capturing: the beep needs the sample clock and must not be recorded.
    beep(1200.0f, 0.1f);
    cfg_.recorder->start_record(*this);
    if (not cfg_.mic->start_capture()) {
        cfg_.recorder->stop_record();
        cfg_.recorder->wait_idle(TIME_S2I(1));
        msg_writer_.close();
        msg_writer_.wait(TIME_S2I(1));
        wf_unlink(msg_path_);
        LOG("recording for \"%s\": microphone did not start", peer.name.data());
        return false;
    }
    recording_peer_ = &peer;
    recording_id_ = id;
    LOG("recording %s for \"%s\" started", msg_path_, peer.name.data());
    return true;
}

void App::finish_recording() {
    if (recording_peer_ == nullptr) {
        return;
    }
    cfg_.mic->stop_capture();
    cfg_.recorder->stop_record();
    beep(800.0f, 0.1f);

    // The last queued frames are encoded after the stop; only then close.
    cfg_.recorder->wait_idle(TIME_S2I(1));
    msg_writer_.close();
    msg_writer_.wait(TIME_S2I(2));
    const FRESULT res = msg_writer_.result();
    const uint32_t size = cfg_.recorder->bytes();
    LOG("recording %s for \"%s\" finished, %u.%us, %u bytes, %u frames dropped, result %d", msg_path_,
        recording_peer_->name.data(), (unsigned) cfg_.recorder->tenths() / 10U,
        (unsigned) cfg_.recorder->tenths() % 10U, (unsigned) size, (unsigned) dropped_frames_, (int) res);

    if (res == FR_OK) {
        if (recording_peer_->tx_msgs.full()) {
            recording_peer_->tx_msgs.pop(1);
        }
        TxMessage &msg = recording_peer_->tx_msgs.write_span(1)[0];
        msg.id = recording_id_;
        msg.size = size;
        msg.flags = 0;
        recording_peer_->tx_msgs.commit(1);
    }
    recording_peer_ = nullptr;

    // Play it back from the file.
    if (res == FR_OK and wf_open(&replay_file_, msg_path_, FA_READ) == FR_OK) {
        replay_open_ = true;
        cfg_.recorder->replay(*cfg_.speaker, *this);
    }
}

void App::on_data_frame_sent(Address addr) {
    Peer *peer = get_peer(addr);
    if (peer) {
        send_messages(*peer);
    }
}

void App::on_data_frame_received(Address addr, std::span<const uint8_t> frame) {
    uint8_t msg_type = frame[0];
    proto::app::dispatch_message(msg_type, messgen::bytes{&frame[1], frame.size() - 1}, [&](auto &&recv_msg) {
        using ActualType = std::decay_t<decltype(recv_msg)>;
        using DataType = ActualType::data_type;
        DataType data;
        if (recv_msg.deserialize(data)) {
            Peer *peer = get_peer(addr);
            if (peer) {
                on_message(*peer, data);
            }
        }
    });
}

void App::on_melpe_frame(std::span<const uint8_t> frame) {
    // Whole frames only, so a full buffer drops a frame rather than misaligning the stream.
    if (msg_writer_.free() < frame.size() or msg_writer_.write(frame) != (ssize_t) frame.size()) {
        dropped_frames_ = dropped_frames_ + 1U;
    }
}

bool App::fill_melpe_frame(std::span<uint8_t> frame) {
    if (not replay_open_) {
        return false;
    }
    UINT got = 0;
    if (wf_read(&replay_file_, frame.data(), frame.size(), &got) == FR_OK and got == frame.size()) {
        return true;
    }
    wf_close(&replay_file_);
    replay_open_ = false;
    return false;
}

void App::send_messages(Peer &peer) {
    if (mac().is_busy(peer.addr)) {
        return;
    }
    for (TxMessage &msg: peer.tx_msgs) {
        if (msg.flags & TxMessage::acked) {
            // Already delivered
            continue;
        }
        if (not(msg.flags & TxMessage::notified)) {
            // Not notified yet
            types::app::message_new msg_new;
            msg_new.id = peer.addr;
            msg_new.size = msg.size;
            msg_new.msg_type = types::app::message_type::voice;
            mac().send_data_frame(peer.addr, proto::app::message_new::send(&msg_new));
            break;
        }
    }
}

void App::on_message(Peer &peer, const types::app::message_new &msg_new) {}

void App::on_message(Peer &peer, const types::app::message_get &msg_get) {}

void App::on_message(Peer &peer, const types::app::message_data &msg_data) {}

void App::beep(float freq_hz, float duration_s) {
    audio::DACSpeaker &spk = *cfg_.speaker;
    spk.stop_stream();
    tone_gen.tone(spk, freq_hz, 0.3f, duration_s);
    spk.wait_idle(TIME_S2I(1));
}

void App::do_set_backlight(int brightness) { pwmEnableChannel(cfg_.backlight_pwm, cfg_.backlight_ch, brightness); }

}// namespace pika::app
