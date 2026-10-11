#include "App.h"

#include "chprintf.h"
#include "ff.h"

#include <charconv>
#include <cstring>
#include <string_view>

namespace pika {
namespace {

std::string_view trim(std::string_view s) {
    constexpr std::string_view space = " \t\r\n";
    const size_t first = s.find_first_not_of(space);
    if (first == std::string_view::npos) {
        return {};
    }
    return s.substr(first, s.find_last_not_of(space) - first + 1);
}

// "<name>,<key as hex>"; the name may itself contain commas.
bool parse_contact(std::string_view line, Contact &contact) {
    const size_t comma = line.rfind(',');
    if (comma == std::string_view::npos) {
        return false;
    }
    const std::string_view name = trim(line.substr(0, comma));
    const std::string_view key = trim(line.substr(comma + 1));

    if (name.empty() or name.size() >= contact.name.size() or key.size() != contact.key.size() * 2) {
        return false;
    }
    for (size_t i = 0; i < contact.key.size(); i++) {
        const char *byte = key.data() + 2 * i;
        const auto [end, ec] = std::from_chars(byte, byte + 2, contact.key[i], 16);
        if (ec != std::errc{} or end != byte + 2) {
            return false;
        }
    }
    *std::copy(name.begin(), name.end(), contact.name.begin()) = '\0';
    return true;
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
    auto contacts = app_->contacts();
    for (size_t i = 0; i < contacts.size(); ++i) {
        auto &c = app_->contacts()[i];
        contacts_items_[i].name = c.name.data();
        contacts_items_[i].on_activate = [](void *) {};
    }
    menu_.set_items({contacts_items_.data(), contacts.size()});
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

void App::load_contacts() {
    // Static: too big for a thread stack.
    static FIL file;
    static char line[200];

    contacts_num_ = 0;
    FRESULT res = f_open(&file, "/contacts.txt", FA_READ);
    if (res != FR_OK) {
        LOG("contacts: cannot open contacts.txt, error %d", (int) res);
        return;
    }
    unsigned line_no = 0;
    while (f_gets(line, sizeof(line), &file) != nullptr) {
        line_no++;
        size_t len = strlen(line);
        if (line[len - 1] != '\n' and not f_eof(&file)) {
            LOG("contacts: line %u too long, ignored", line_no);
            while (f_gets(line, sizeof(line), &file) != nullptr and line[strlen(line) - 1] != '\n') {}
            continue;
        }
        const std::string_view text = trim(line);
        if (text.empty() or text.front() == '#') {
            continue;
        }
        if (contacts_num_ == contacts_.size()) {
            LOG("contacts: more than %u, rest ignored", (unsigned) contacts_.size());
            break;
        }
        if (parse_contact(text, contacts_[contacts_num_])) {
            contacts_num_++;
        } else {
            LOG("contacts: line %u malformed, ignored", line_no);
        }
    }
    if (f_error(&file)) {
        LOG("contacts: read error");
    }
    f_close(&file);
    LOG("contacts: %u loaded", (unsigned) contacts_num_);
}

void HomeScreen::record_message_start() {
    if (menu_.empty()) {
        return;
    }
    const Contact &contact = app_->contacts()[menu_.cursor()];
    app_->start_recording(contact);
    recording_screen_.show(contact);
}

void RecordingScreen::show(const Contact &contact) {
    contact_ = &contact;
    init_activate();
}

void RecordingScreen::draw() {
    app_->canvas().text(4, 30, "Recording message:");
    app_->canvas().text(4, 50, contact_->name.data());
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
    load_contacts();
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

void App::start_recording(const Contact &contact) {
    // Before capturing: the beep needs the sample clock and must not be recorded.
    beep(1200.0f, 0.1f);
    cfg_.recorder->start_record();
    if (not cfg_.mic->start_capture()) {
        cfg_.recorder->stop_record();
        LOG("recording for \"%s\": microphone did not start", contact.name.data());
        return;
    }
    recording_for_ = &contact;
    LOG("recording message for \"%s\" started", contact.name.data());
}

void App::finish_recording() {
    if (recording_for_ == nullptr) {
        return;
    }
    cfg_.mic->stop_capture();
    cfg_.recorder->stop_record();
    beep(800.0f, 0.1f);
    LOG("recording message for \"%s\" finished, %u.%us, %u bytes", recording_for_->name.data(),
        (unsigned) cfg_.recorder->tenths() / 10U, (unsigned) cfg_.recorder->tenths() % 10U,
        (unsigned) cfg_.recorder->bytes());
    recording_for_ = nullptr;
    cfg_.recorder->replay(*cfg_.speaker);
}

void App::beep(float freq_hz, float duration_s) {
    audio::DACSpeaker &spk = *cfg_.speaker;
    spk.stop_stream();
    tone_gen.tone(spk, freq_hz, 0.3f, duration_s);
    spk.wait_idle(TIME_S2I(1));
}

void App::do_set_backlight(int brightness) { pwmEnableChannel(cfg_.backlight_pwm, cfg_.backlight_ch, brightness); }

}// namespace pika
