#include "ui.h"

#include "App.h"

#include <pika/lcd/images/pika_logo.h>
#include <pika/log.h>

#include "chprintf.h"

namespace pika::app {


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

}// namespace pika::app
