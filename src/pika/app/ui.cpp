#include "ui.h"

#include "App.h"

#include <pika/lcd/images/pika_logo.h>
#include <pika/log.h>

#include "chprintf.h"

#include <cstring>

namespace pika::app {
namespace {

// A filled dot.
constexpr uint8_t online_icon[8] = {0x00, 0x3C, 0x7E, 0x7E, 0x7E, 0x7E, 0x3C, 0x00};

}// namespace

void StatusBar::draw() {
    app_->canvas().rect<false>(0, 0, 160, status_bar_h);
    char buf[20];
    chsnprintf(buf, sizeof(buf), "%s %.2f", app_->battery_status().charging != Charge::off ? "CHG" : "   ",
               app_->battery_status().voltage / 1000.0f);
    app_->canvas().text<true>(2, 0, buf);

    const bool tx = app_->mac().tx_state() == RadioState::tx;
    app_->canvas().rect<false>(160 - 2 - 2 * 8, 0, 16, 8);
    switch (app_->mac().tx_state()) {
        case RadioState::tx:
            app_->canvas().text<true>(160 - 2 - 1 * 8, 0, "T");
            break;
        case RadioState::rx:
            app_->canvas().text<true>(160 - 2 - 2 * 8, 0, "R");
            break;
        default:
            break;
    }
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

    for (size_t i = 0; i < items_.size(); i++) { draw_item(i); }
    draw_cursor(true);
}

void Menu::draw_item(size_t i) {
    const int y = (int) i * row_h + items_y;
    app_->canvas().rect<false>(label_x, y, 160 - label_x, row_h);
    app_->canvas().text(label_x, y, items_[i].name);
    // The value and the icon go over the name, so a long one ends where they
    // start; the icon's place is kept even without one.
    if (items_[i].value != nullptr) {
        const int w = 8 * (int) strlen(items_[i].value);
        const int x = icon_x - 2 - w;
        app_->canvas().rect<false>(x - 2, y, w + 4, icon_size);
        app_->canvas().text(x, y, items_[i].value);
    }
    app_->canvas().rect<false>(icon_x, y, icon_size, icon_size);
    if (items_[i].icon != nullptr) {
        app_->canvas().bitmap(icon_x, y, icon_size, icon_size, items_[i].icon);
    }
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
        update_item(i);
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
        case input::Button::lsn:
            if (not menu_.empty()) {
                app_->play_last_message(menu_.cursor());
            }
            return;
        case input::Button::pwr:
            main_menu_screen_.init_activate();
            return;
        default:
            break;
    }
    menu_.on_press(button);
}

void HomeScreen::on_peer_updated(Address peer_addr) {
    auto peers = app_->peers();
    for (size_t i = 0; i < peers.size(); ++i) {
        if (peers[i].addr != peer_addr) {
            continue;
        }
        update_item(i);
        // Otherwise drawn with the rest when this screen is shown again.
        if (app_->is_active(*this)) {
            menu_.draw_item(i);
        }
    }
}

void HomeScreen::update_item(size_t i) {
    const Peer &peer = app_->peers()[i];
    peer_items_[i].icon = app_->mac().is_online(peer.addr) ? online_icon : nullptr;
    bool tx_pending = false;
    for (auto &tx_msg: peer.tx_msgs) {
        if (not(tx_msg.flags & TxMessage::acked)) {
            tx_pending = true;
        }
    }
    bool rx_pending = false;
    for (auto &rx_msg: peer.rx_msgs) {
        if (rx_msg.size_received < rx_msg.size) {
            rx_pending = true;
        }
    }

    chsnprintf(peer_counts_[i].data(), peer_counts_[i].size(), "%i %c%c", int(peer.rssi), tx_pending ? 'T' : ' ',
               rx_pending ? 'R' : ' ');
    peer_items_[i].value = peer_counts_[i].data();
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
