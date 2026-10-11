#include "App.h"

#include <pika/fatfs_wrapper/fatfsWrapper.h>
#include <pika/lcd/images/pika_logo.h>

#include "chprintf.h"
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
    const auto [id_end, id_ec] = std::from_chars(id.data(), id.data() + id.size(), peer.id, 16);
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

void StatusBar::draw() {
    app_->canvas().rect<false>(0, 0, 160, status_bar_h);
    char buf[20];
    chsnprintf(buf, sizeof(buf), "%s %.2f", app_->battery_status().charging != Charge::off ? "CHG" : "   ",
               app_->battery_status().voltage / 1000.0f);
    app_->canvas().text<true>(2, 0, buf);
}

bool Screen::handle_back(input::Button button) {
    if (button != input::Button::left or parent_screen_ == nullptr) {
        return false;
    }
    app_->activate_screen(*parent_screen_);
    return true;
}

void Menu::draw() {
    app_->canvas().rect<true>(0, title_y, 160, title_h);
    app_->canvas().text<false>(2, title_y + 2, title_);

    for (size_t i = 0; i < items_.size(); i++) {
        app_->canvas().text(label_x, (int) i * row_h + items_y, items_[i].name);
    }
    draw_cursor(true);
}

void Menu::draw_cursor(bool on) {
    if (items_.empty()) {
        return;
    }
    const int y = cursor_ * row_h + items_y;
    if (on) {
        app_->canvas().text<true>(4, y, ">");
    } else {
        app_->canvas().text<false>(4, y, ">");
    }
}

void Menu::move_cursor(int cursor) {
    draw_cursor(false);
    cursor_ = cursor;
    draw_cursor(true);
}

void Screen::init_activate() {
    init();
    app_->activate_screen(*this);
}

bool Menu::on_press(input::Button button) {
    const int last = (int) items_.size() - 1;
    switch (button) {
        case input::Button::up:
            if (cursor_ > 0) {
                move_cursor(cursor_ - 1);
            }
            return true;
        case input::Button::down:
            if (cursor_ < last) {
                move_cursor(cursor_ + 1);
            }
            return true;
        case input::Button::right:
            if (cursor_ <= last and items_[cursor_].on_activate != nullptr) {
                items_[cursor_].on_activate(owner_);
            }
            return true;
        default:
            return false;
    }
}

void AboutScreen::draw() {
    app_->canvas().clear();
    app_->canvas().bitmap(0, 0, pika::lcd::pika_logo_width, pika::lcd::pika_logo_height, pika::lcd::pika_logo);
    app_->canvas().text(80, 92, BOARD_NAME);
}

void AboutScreen::on_press(input::Button) {
    // Return back on any button
    app_->activate_screen(*parent_screen_);
}

void SettingsScreen::draw() { menu_.draw(); }

void SettingsScreen::on_press(input::Button button) {
    if (not menu_.on_press(button)) {
        handle_back(button);
    }
}

void SettingsScreen::set_backlight() {
    int b = (app_->get_backlight() / backlight_step) + 1;
    if (b > backlight_steps_num) {
        b = 0;
    }
    app_->set_backlight(b * backlight_step);
}

void MainMenuScreen::draw() { menu_.draw(); }

void MainMenuScreen::on_press(input::Button button) {
    if (not menu_.on_press(button)) {
        handle_back(button);
    }
}

void HomeScreen::init() {
    auto peers = app_->peers();
    for (size_t i = 0; i < peers.size(); ++i) {
        auto &c = app_->peers()[i];
        peer_items_[i].name = c.name.data();
        peer_items_[i].on_activate = [](void *) {};
    }
    menu_.set_items({peer_items_.data(), peers.size()});
    menu_.reset();
}

void HomeScreen::draw() { menu_.draw(); }

void HomeScreen::on_press(input::Button button) {
    switch (button) {
        case input::Button::ptt:
            record_message_start();
            return;
        case input::Button::pwr:
            main_menu_screen_.init_activate();
            return;
        default:
            break;
    }
    menu_.on_press(button);
}

void App::load_peers() {
    // Static: too big for a thread stack.
    static FIL file;
    static char line[200];

    peers_num_ = 0;
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
        if (peers_num_ == peers_.size()) {
            LOG("peers: more than %u, rest ignored", (unsigned) peers_.size());
            break;
        }
        if (parse_peer(text, peers_[peers_num_])) {
            peers_num_++;
        } else {
            LOG("peers: line %u malformed, ignored", line_no);
        }
    }
    if (wf_error(&file)) {
        LOG("peers: read error");
    }
    wf_close(&file);
    LOG("peers: %u loaded", (unsigned) peers_num_);
}

void HomeScreen::record_message_start() {
    if (menu_.empty()) {
        return;
    }
    const size_t peer = menu_.cursor();
    if (app_->start_recording(peer)) {
        recording_screen_.show(app_->peers()[peer]);
    }
}

void RecordingScreen::show(const Peer &peer) {
    peer_ = &peer;
    init_activate();
}

void RecordingScreen::draw() {
    app_->canvas().text(4, 30, "Recording message:");
    app_->canvas().text(4, 50, peer_->name.data());
}

void RecordingScreen::on_release(input::Button button) {
    if (button != input::Button::ptt) {
        return;
    }
    app_->finish_recording();
    app_->activate_screen(*parent_screen_);
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
    if (recording_for_ != nullptr or peer_index >= peers_num_ or not cfg_.recorder->idle()) {
        return false;
    }
    Peer &peer = peers_[peer_index];

    // "/<peer id>/tx/<message id>", creating the directories on the way.
    char *p = msg_path_;
    *p++ = '/';
    p = put_hex(p, peer.id, 16);
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
    recording_for_ = &peer;
    recording_id_ = id;
    LOG("recording %s for \"%s\" started", msg_path_, peer.name.data());
    return true;
}

void App::finish_recording() {
    if (recording_for_ == nullptr) {
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
        recording_for_->name.data(), (unsigned) cfg_.recorder->tenths() / 10U, (unsigned) cfg_.recorder->tenths() % 10U,
        (unsigned) size, (unsigned) dropped_frames_, (int) res);

    if (res == FR_OK) {
        if (recording_for_->tx_msgs.full()) {
            LOG("recording: tx queue of \"%s\" full", recording_for_->name.data());
        } else {
            recording_for_->tx_msgs.write_span(1)[0] = Message{recording_id_, size};
            recording_for_->tx_msgs.commit(1);
        }
    }
    recording_for_ = nullptr;

    // Play it back from the file.
    if (res == FR_OK and wf_open(&replay_file_, msg_path_, FA_READ) == FR_OK) {
        replay_open_ = true;
        cfg_.recorder->replay(*cfg_.speaker, *this);
    }
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

void App::beep(float freq_hz, float duration_s) {
    audio::DACSpeaker &spk = *cfg_.speaker;
    spk.stop_stream();
    tone_gen.tone(spk, freq_hz, 0.3f, duration_s);
    spk.wait_idle(TIME_S2I(1));
}

void App::do_set_backlight(int brightness) { pwmEnableChannel(cfg_.backlight_pwm, cfg_.backlight_ch, brightness); }

}// namespace pika::app
