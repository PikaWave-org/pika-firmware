#include "App.h"

#include <pika/fatfs_wrapper/fatfsWrapper.h>

#include "chprintf.h"
#include "ff.h"

#include <algorithm>
#include <charconv>
#include <cmath>
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

// "<id as hex>,<name>,<key as hex>" into a Peer or an Identity; the name may
// itself contain commas. A key shorter than the field is zero filled.
template<class T>
bool parse_peer(std::string_view line, T &peer) {
    const size_t first_comma = line.find(',');
    const size_t last_comma = line.rfind(',');
    if (first_comma == std::string_view::npos or first_comma == last_comma) {
        return false;
    }
    const std::string_view id = trim(line.substr(0, first_comma));
    const std::string_view name = trim(line.substr(first_comma + 1, last_comma - first_comma - 1));
    const std::string_view key = trim(line.substr(last_comma + 1));

    if (id.empty() or id.size() > 16U or name.empty() or name.size() >= peer.name.size() or key.empty() or
        key.size() % 2U != 0U or key.size() > peer.key.size() * 2) {
        return false;
    }
    const auto [id_end, id_ec] = std::from_chars(id.data(), id.data() + id.size(), peer.addr, 16);
    if (id_ec != std::errc{} or id_end != id.data() + id.size()) {
        return false;
    }
    peer.key.fill(0);
    for (size_t i = 0; i < key.size() / 2U; i++) {
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

// "/<peer id>/<dir>/<message id>", 31 characters at most for a 2 letter dir.
void put_msg_path(char *path, Address peer_addr, const char *dir, uint32_t msg_id) {
    char *p = path;
    *p++ = '/';
    p = put_hex(p, peer_addr, 16);
    *p++ = '/';
    p = std::copy_n(dir, 2, p);
    *p++ = '/';
    p = put_hex(p, msg_id, 8);
    *p = '\0';
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

void App::load_identity() {
    // Static: too big for the stack init() runs on.
    static FIL file;
    static char line[256];

    const FRESULT res = wf_open(&file, "/id.txt", FA_READ);
    if (res != FR_OK) {
        LOG("id: cannot open id.txt, error %d, keeping the default address", (int) res);
        return;
    }
    const bool got = wf_gets(line, sizeof(line), &file) != nullptr;
    wf_close(&file);

    if (not got or not parse_peer(trim(line), self_)) {
        LOG("id: id.txt malformed, keeping the default address");
        return;
    }
    cfg_.mac->set_address(self_.addr);
    LOG("id: " ADDR_FMT " \"%s\"", ADDR_ARG(self_.addr), self_.name.data());
}

void App::load_settings() {
    // Static: too big for the stack init() runs on.
    static FIL file;
    static char line[64];

    const FRESULT res = wf_open(&file, "/settings.txt", FA_READ);
    if (res != FR_OK) {
        LOG("settings: cannot open settings.txt, error %d, keeping defaults", (int) res);
        return;
    }
    unsigned line_no = 0;
    while (wf_gets(line, sizeof(line), &file) != nullptr) {
        line_no++;
        const std::string_view text = trim(line);
        if (text.empty() or text.front() == '#') {
            continue;
        }
        const size_t eq = text.find('=');
        const std::string_view key = trim(text.substr(0, eq));
        const std::string_view value = eq == std::string_view::npos ? std::string_view{} : trim(text.substr(eq + 1));
        int v = 0;
        const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), v);
        if (value.empty() or ec != std::errc{} or end != value.data() + value.size()) {
            LOG("settings: line %u malformed, ignored", line_no);
        } else if (key == "backlight") {
            set_backlight(std::clamp(v, 0, 100) * 10);
        } else if (key == "tx_power") {
            cfg_.mac->set_tx_power((float) v);
        } else {
            LOG("settings: line %u has an unknown key, ignored", line_no);
        }
    }
    wf_close(&file);
}

void App::save_settings() {
    // Static: too big for a thread stack.
    static FIL file;

    char text[48];
    const int n = chsnprintf(text, sizeof(text), "backlight=%d\ntx_power=%d\n", backlight_brightness_ / 10,
                             (int) lroundf(cfg_.mac->tx_power()));
    FRESULT res = wf_open(&file, "/settings.txt", FA_WRITE | FA_CREATE_ALWAYS);
    if (res == FR_OK) {
        UINT written = 0;
        res = wf_write(&file, text, (UINT) n, &written);
        const FRESULT closed = wf_close(&file);
        if (res == FR_OK) {
            res = written == (UINT) n ? closed : FR_DENIED;
        }
    }
    if (res != FR_OK) {
        LOG("settings: cannot save settings.txt, error %d", (int) res);
    }
}

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
    event_listener_t btn_listener;
    event_listener_t mac_listener;
    event_listener_t frame_listener;
    event_listener_t recorder_listener;
    chEvtRegister(&buttons_.events(), &btn_listener, 0);
    chEvtRegister(&cfg_.mac->events(), &mac_listener, 1);
    chEvtRegister(&frame_event_src_, &frame_listener, 2);
    chEvtRegister(&cfg_.recorder->idle_events(), &recorder_listener, 3);

    while (true) {
        // Shorter while LEFT is held, so a long press locks on time rather than on release.
        const bool may_lock = not locked_ and buttons_.pressed(input::Button::left);
        const sysinterval_t timeout = may_lock ? TIME_MS2I(50) : TIME_MS2I(500);
        const eventmask_t events =
                chEvtWaitAnyTimeout(EVENT_MASK(0) | EVENT_MASK(1) | EVENT_MASK(2) | EVENT_MASK(3), timeout);
        if (events & EVENT_MASK(3)) {
            on_recorder_idle();
        }
        if (events & EVENT_MASK(1)) {
            // Transmission started or ended: the status bar below shows it.
            chEvtGetAndClearFlags(&mac_listener);
        }
        // Every pass, not only on the event: the MAC may queue some before this thread registers.
        handle_frame_events();
        if (events & EVENT_MASK(0)) {
            systime_t now = chVTGetSystemTime();
            const eventflags_t flags = chEvtGetAndClearFlags(&btn_listener);
            for (unsigned i = 0; i < input::Buttons::count; i++) {
                const auto btn = (input::Button) i;
                const bool press = (flags & buttons_.press_flag(btn)) != 0U;
                const bool release = (flags & buttons_.release_flag(btn)) != 0U;

                if ((not press and not release) or active_screen_ == nullptr) {
                    continue;
                }
                backlight_wake_ = now;
                // A quick tap can bring both; the press comes first.
                if (press and not lock_press(btn, now)) {
                    active_screen_->on_press(btn);
                }
                if (release) {
                    active_screen_->on_release(btn);
                }
                cfg_.display->flush();
            }
        }
        check_lock_hold(chVTGetSystemTime());
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

bool App::lock_press(input::Button btn, systime_t now) {
    const bool locked = locked_;
    if (locked and btn == input::Button::right and last_press_ == input::Button::left and
        chTimeDiffX(last_press_at_, now) <= unlock_window) {
        locked_ = false;
        beep(1000.0f, 0.1f);
    }
    last_press_ = btn;
    last_press_at_ = now;
    return locked;
}

void App::check_lock_hold(systime_t now) {
    if (not locked_ and last_press_ == input::Button::left and buttons_.pressed(input::Button::left) and
        chTimeDiffX(last_press_at_, now) >= lock_hold) {
        locked_ = true;
        beep(600.0f, 0.2f);
    }
}

void App::init() {
    LOG("app init");
    msg_writer_.start(NORMALPRIO);
    // Before the MAC starts.
    cfg_.mac->set_frame_handler(this);
    load_identity();
    load_peers();
    set_backlight(500);
    load_settings();
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
        home_screen_.on_peer_updated(*recording_peer_);
        LOG("tx message added: id=%u", msg.id);
        send_messages(*recording_peer_);
    }
    recording_peer_ = nullptr;
}

void App::on_data_frame_sent(Address addr, const PeerInfo &peer_info) {
    post_frame_event(MACEvent::sent, addr, peer_info, {});
}

void App::on_data_frame_received(Address addr, std::span<const uint8_t> data, const PeerInfo &peer_info) {
    post_frame_event(MACEvent::received, addr, peer_info, data);
}

void App::on_peer_updated(Address addr, const PeerInfo &peer_info) {
    post_frame_event(MACEvent::peer_updated, addr, peer_info, {});
}

void App::post_frame_event(MACEvent::Kind kind, Address addr, const PeerInfo &peer_info,
                           std::span<const uint8_t> data) {
    if (frame_events_.full()) {
        LOG("WARNING: frame event: %s from " ADDR_FMT " dropped, queue full", MACEvent::name(kind), ADDR_ARG(addr));
        return;
    }

    MACEvent &event = frame_events_.write_span(1)[0];
    event.kind = kind;
    event.addr = addr;
    event.data.resize(data.size());
    event.peer_info = peer_info;
    std::copy(data.begin(), data.end(), event.data.begin());
    frame_events_.commit(1);
    chEvtBroadcast(&frame_event_src_);
}

void App::handle_frame_events() {
    while (not frame_events_.empty()) {
        MACEvent &event = frame_events_.read_span(1)[0];
        Peer *peer = get_peer(event.addr);
        // LOG("frame event: %s from " ADDR_FMT " \"%s\", %u bytes", FrameEvent::name(event.kind), ADDR_ARG(event.addr),
        //     peer ? peer->name.data() : "unknown peer", (unsigned) event.frame.size());
        if (peer != nullptr) {
            peer->info = event.peer_info;
        }
        switch (event.kind) {
            case MACEvent::sent:
                if (peer) {
                    send_messages(*peer);
                }
                break;

            case MACEvent::received: {
                const std::span<const uint8_t> frame{event.data.data(), event.data.size()};
                if (frame.empty()) {
                    break;
                }
                proto::app::dispatch_message(
                        frame[0], messgen::bytes{&frame[1], frame.size() - 1}, [&](auto &&recv_msg) {
                            using ActualType = std::decay_t<decltype(recv_msg)>;
                            using DataType = ActualType::data_type;
                            DataType data;
                            // Bytes fields point into the frame, alive until the pop below.
                            if (recv_msg.deserialize(data, messgen::empty_alloc, messgen::NoCopy{}) < 0) {
                                LOG("ERROR: frame event: message type %u malformed", (unsigned) frame[0]);
                            } else if (peer) {
                                on_message(*peer, data);
                            }
                        });
                break;
            }

            case MACEvent::peer_updated:
                if (peer != nullptr) {
                    home_screen_.on_peer_updated(*peer);
                }
                break;
        }
        frame_events_.pop(1);
        // A handler may have redrawn a row; only changed pages are sent.
        cfg_.display->flush();
    }
}

void App::on_melpe_frame(std::span<const uint8_t> frame) {
    // Whole frames only, so a full buffer drops a frame rather than misaligning the stream.
    if (msg_writer_.free() < frame.size() or msg_writer_.write(frame) != (ssize_t) frame.size()) {
        dropped_frames_ = dropped_frames_ + 1U;
    }
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
        if (not(msg.flags & TxMessage::notified) or (now() > msg.last_req_time + mac().retransmit_delay())) {
            // Not notified yet or didn't got ack
            types::app::message_new msg_new;
            msg_new.id = msg.id;
            msg_new.size = msg.size;
            msg_new.msg_type = types::app::message_type::voice;
            mac().send_data_frame(peer.addr, proto::app::message_new::send(&msg_new));
            break;
        }
    }
    for (RxMessage &msg: peer.rx_msgs) {
        if (msg.size_received < msg.size and now() > msg.last_req_time + mac().retransmit_delay()) {
            LOG("retransmit request msg");
            request_message(peer, msg);
        }
    }
}

void App::on_message(Peer &peer, const types::app::message_new &msg_new) {
    LOG("app rx <- " ADDR_FMT " \"%s\": message_new id=%u size=%u type=%u", ADDR_ARG(peer.addr), peer.name.data(),
        (unsigned) msg_new.id, (unsigned) msg_new.size, (unsigned) msg_new.msg_type);

    RxMessage *msg = nullptr;
    // Find message in buffer
    for (RxMessage &m: peer.rx_msgs) {
        if (m.id == msg_new.id) {
            msg = &m;
            break;
        }
    }

    if (msg == nullptr) {
        // Message not found
        if (peer.rx_msgs.full()) {
            // Receive buffer is full, discard old one
            peer.rx_msgs.pop(1);
        }

        // Add new message to buffer
        msg = &peer.rx_msgs.write_span(1)[0];
        msg->id = msg_new.id;
        msg->size = msg_new.size;
        msg->size_received = 0;
        msg->msg_type = msg_new.msg_type;
        peer.rx_msgs.commit(1);
        home_screen_.on_peer_updated(peer);
    } else {
        // Update message info
        msg->size = msg_new.size;
        msg->msg_type = msg_new.msg_type;
    }

    beep(600.0f, 0.05f);

    request_message(peer, *msg);
}

void App::request_message(Peer &peer, RxMessage &msg) {
    if (msg.msg_type == types::app::message_type::none) {
        return;
    }
    types::app::message_get msg_get;
    msg_get.id = msg.id;
    msg_get.offset = msg.size_received;
    LOG("app tx -> " ADDR_FMT " \"%s\": message_get id=%u offset=%u", ADDR_ARG(peer.addr), peer.name.data(),
        (unsigned) msg_get.id, (unsigned) msg_get.offset);
    if (mac().send_data_frame(peer.addr, proto::app::message_get::send(&msg_get))) {
        msg.last_req_time = now();
    } else {
        LOG("app tx -> " ADDR_FMT ": message_get not sent, MAC busy", ADDR_ARG(peer.addr));
    }
}

bool App::write_message(Address peer_addr, uint32_t msg_id, uint32_t offset, std::span<const uint8_t> data) {
    // "/<peer id>/rx/<message id>", creating the directories with the file.
    char path[32];
    char *p = path;
    *p++ = '/';
    p = put_hex(p, peer_addr, 16);
    *p = '\0';
    if (offset == 0) {
        wf_mkdir(path);
    }
    p = std::copy_n("/rx", 3, p);
    *p = '\0';
    if (offset == 0) {
        wf_mkdir(path);
    }
    *p++ = '/';
    p = put_hex(p, msg_id, 8);
    *p = '\0';

    if (wf_open(&msg_file_, path, FA_WRITE | (offset == 0 ? FA_CREATE_ALWAYS : FA_OPEN_EXISTING)) != FR_OK) {
        return false;
    }
    UINT written = 0;
    const bool ok = wf_lseek(&msg_file_, offset) == FR_OK and
                    wf_write(&msg_file_, data.data(), data.size(), &written) == FR_OK and written == data.size();
    return wf_close(&msg_file_) == FR_OK and ok;
}

bool App::read_message(Address peer_addr, uint32_t msg_id, uint32_t offset, size_t size,
                       FixedVector<uint8_t, 256> &buf) {
    buf.clear();
    if (size > buf.cap()) {
        return false;
    }
    char path[32];
    put_msg_path(path, peer_addr, "tx", msg_id);

    if (wf_open(&msg_file_, path, FA_READ) != FR_OK) {
        return false;
    }
    UINT got = 0;
    const bool ok = wf_lseek(&msg_file_, offset) == FR_OK and wf_read(&msg_file_, buf.data(), size, &got) == FR_OK and
                    got == size;
    wf_close(&msg_file_);
    if (ok) {
        buf.resize(size);
    }
    return ok;
}

void App::on_message(Peer &peer, const types::app::message_get &msg_get) {
    LOG("app rx <- " ADDR_FMT " \"%s\": message_get id=%u offset=%u", ADDR_ARG(peer.addr), peer.name.data(),
        (unsigned) msg_get.id, (unsigned) msg_get.offset);
    for (TxMessage &msg: peer.tx_msgs) {
        if (msg.id == msg_get.id) {
            // Peer requests data, notification acked
            msg.flags |= TxMessage::notified;
            msg.last_req_time = now();

            // Send requested data
            types::app::message_data msg_data;
            msg_data.id = msg_get.id;
            msg_data.offset = msg_get.offset;
            ssize_t size = std::min(200, (ssize_t) msg.size - (ssize_t) msg_get.offset);
            if (size == 0) {
                // Receiver ack for full message
                if (not(msg.flags & TxMessage::acked)) {
                    LOG("msg acked id=%u", msg.id);
                    msg.flags |= TxMessage::acked;
                    home_screen_.on_peer_updated(peer);
                    beep(1000.0f, 0.1f);
                    beep(600.0f, 0.1f);
                }
            } else if (size > 0) {
                bool ok = read_message(peer.addr, msg.id, msg_get.offset, size, tx_buf_);
                if (ok) {
                    LOG("app tx -> " ADDR_FMT " \"%s\": message_data id=%u offset=%u", ADDR_ARG(peer.addr),
                        peer.name.data(), (unsigned) msg_data.id, (unsigned) msg_data.offset);
                    msg_data.data = messgen::bytes(tx_buf_.data(), tx_buf_.size());
                    mac().send_data_frame(peer.addr, proto::app::message_data::send(&msg_data));
                } else {
                    LOG("ERROR reading msg addr=" ADDR_FMT " id=%u offs=%u size=%i from file", ADDR_ARG(peer.addr),
                        (unsigned) msg.id, (unsigned) msg_get.offset, (int) size);
                }
            }

            return;
        }
    }
    LOG("requested message not found id=%u", msg_get.id);
    // Send dummy message_new to stop the spam
    types::app::message_new msg_new;
    msg_new.id = msg_get.id;
    msg_new.size = 0;
    msg_new.msg_type = types::app::message_type::none;
    mac().send_data_frame(peer.addr, proto::app::message_new::send(&msg_new));
}

void App::on_message(Peer &peer, const types::app::message_data &msg_data) {
    LOG("app rx <- " ADDR_FMT " \"%s\": message_data id=%u offset=%u size=%u", ADDR_ARG(peer.addr), peer.name.data(),
        (unsigned) msg_data.id, (unsigned) msg_data.offset, (unsigned) msg_data.data.size());

    RxMessage *msg = nullptr;
    for (RxMessage &m: peer.rx_msgs) {
        if (m.id == msg_data.id) {
            msg = &m;
            break;
        }
    }
    if (msg == nullptr) {
        LOG("app: message %u from \"%s\" not announced, chunk ignored", (unsigned) msg_data.id, peer.name.data());
        return;
    }

    // A chunk other than the one asked for (a repeat, or past the end) is
    // dropped and the wanted one asked for again.
    const std::span<const uint8_t> data{msg_data.data.data(), msg_data.data.size()};
    bool fully_received = false;
    if (msg_data.offset == msg->size_received and not data.empty() and data.size() <= msg->size - msg->size_received) {
        if (not write_message(peer.addr, msg->id, msg_data.offset, data)) {
            LOG("app: message %u from \"%s\": cannot write chunk at %u", (unsigned) msg->id, peer.name.data(),
                (unsigned) msg_data.offset);
            return;
        }
        msg->size_received += data.size();
        msg->last_req_time = now();
        if (msg->size_received == msg->size) {
            fully_received = true;
        } else {
            beep(600.0f, 0.05f);
        }
    } else {
        LOG("app: message %u from \"%s\": unexpected chunk at %u, want %u", (unsigned) msg->id, peer.name.data(),
            (unsigned) msg_data.offset, (unsigned) msg->size_received);
    }

    request_message(peer, *msg);

    if (fully_received) {
        home_screen_.on_peer_updated(peer);
        LOG("app: message %u from \"%s\" received, %u bytes", (unsigned) msg->id, peer.name.data(),
            (unsigned) msg->size);
        play_message(peer, *msg);
    }
}

bool App::play_last_message(size_t peer_index) {
    if (peer_index >= peers_.size()) {
        return false;
    }
    const Peer &peer = peers_[peer_index];
    // Oldest first, so the last complete one wins.
    const RxMessage *last = nullptr;
    for (const RxMessage &msg: peer.rx_msgs) {
        if (msg.size > 0 and msg.size_received == msg.size) {
            last = &msg;
        }
    }
    if (last == nullptr) {
        LOG("app: no received message from \"%s\" to play", peer.name.data());
        return false;
    }
    return play_message(peer, *last);
}

bool App::play_message(const Peer &peer, const RxMessage &msg) {
    // Idle also means the previous replay closed its file.
    if (not cfg_.recorder->idle()) {
        LOG("app: message %u from \"%s\" not played, codec busy", (unsigned) msg.id, peer.name.data());
        return false;
    }
    char path[32];
    put_msg_path(path, peer.addr, "rx", msg.id);
    if (wf_open(&replay_file_, path, FA_READ) != FR_OK) {
        LOG("app: cannot open %s for replay", path);
        return false;
    }
    LOG("app: playing %s", path);
    beep(1200.0f, 0.2f);
    replay_open_ = true;
    replaying_ = true;
    cfg_.recorder->replay(*cfg_.speaker, *this);
    return true;
}

void App::on_recorder_idle() {
    if (not replaying_ or not cfg_.recorder->idle()) {
        return;
    }
    replaying_ = false;
    // The codec is done; the speaker still plays out its last buffer, which
    // the beep would otherwise cut off.
    cfg_.speaker->wait_idle(TIME_MS2I(200));
    beep(800.0f, 0.1f);
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

void App::beep(float freq_hz, float duration_s) {
    audio::DACSpeaker &spk = *cfg_.speaker;
    if (spk.busy() or not cfg_.recorder->idle()) {
        return;
    }
    spk.stop_stream();
    tone_gen.tone(spk, freq_hz, 0.3f, duration_s);
    spk.wait_idle(TIME_S2I(1));
}

void App::do_set_backlight(int brightness) { pwmEnableChannel(cfg_.backlight_pwm, cfg_.backlight_ch, brightness); }

}// namespace pika::app
