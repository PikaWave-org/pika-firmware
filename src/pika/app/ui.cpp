#include "ui.h"

#include "App.h"

#include <pika/lcd/images/pika_logo.h>
#include <pika/log.h>

#include "chprintf.h"

#include <algorithm>
#include <cstring>

namespace pika::app {
namespace {

// A cross.
constexpr uint8_t offline_icon[8] = {0x00, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0x00};

// LoRa SNR in dB to bars, at least one while online. SF7, the firmware's,
// demodulates down to about -7.5dB, so a single bar is close to that edge.
int signal_bars(float snr) {
    if (snr >= 5.0f) {
        return 4;
    }
    if (snr >= 0.0f) {
        return 3;
    }
    if (snr >= -5.0f) {
        return 2;
    }
    return 1;
}

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
    // The value and the slot go over the name, so a long one ends where they
    // start; the slot's place is kept even when empty.
    if (items_[i].value != nullptr) {
        const int w = 8 * (int) strlen(items_[i].value);
        const int x = slot_x - 2 - w;
        app_->canvas().rect<false>(x - 2, y, w + 4, icon_size);
        app_->canvas().text(x, y, items_[i].value);
    }
    app_->canvas().rect<false>(slot_x, y, slot_w, row_h);
    if (items_[i].bars >= 0) {
        draw_bars(y, items_[i].bars);
    } else if (items_[i].icon != nullptr) {
        app_->canvas().bitmap(slot_x + (slot_w - icon_size) / 2, y, icon_size, icon_size, items_[i].icon);
    }
}

// Rising 3, 5, 7 and 9px, bottom aligned one pixel above the next row; an
// unlit bar is its baseline, so all four places show.
void Menu::draw_bars(int y, int lit) {
    for (int bar = 0; bar < 4; bar++) {
        const int h = bar < lit ? 3 + 2 * bar : 1;
        app_->canvas().rect<true>(slot_x + 3 * bar, y + row_h - 1 - h, 2, h);
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
        peer_items_[i].name = peers[i].name.data();
        peer_items_[i].on_activate = [](void *) {};
        update_peer_item(peers[i]);
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

void HomeScreen::on_peer_updated(const Peer &peer) {
    const size_t peer_idx = update_peer_item(peer);
    // Otherwise drawn with the rest when this screen is shown again.
    if (app_->is_active(*this)) {
        menu_.draw_item(peer_idx);
    }
}

size_t HomeScreen::update_peer_item(const Peer &peer) {
    const size_t peer_idx = &peer - app_->peers().data();
    const bool tx_pending = std::any_of(peer.tx_msgs.begin(), peer.tx_msgs.end(),
                                        [](const TxMessage &msg) { return not(msg.flags & TxMessage::acked); });
    const bool rx_pending = std::any_of(peer.rx_msgs.begin(), peer.rx_msgs.end(),
                                        [](const RxMessage &msg) { return msg.size_received < msg.size; });

    MenuItem &item = peer_items_[peer_idx];
    std::array<char, 12> &status = peer_status_[peer_idx];
    const bool online = peer.info.is_online;
    const char tx = tx_pending ? 'T' : ' ';
    const char rx = rx_pending ? 'R' : ' ';
    if (online) {
        chsnprintf(status.data(), status.size(), "%d %c%c", (int) peer.info.radio_stats.snr_packet, tx, rx);
    } else {
        chsnprintf(status.data(), status.size(), "%c%c", tx, rx);
    }
    item.value = status.data();
    item.bars = online ? (int8_t) signal_bars(peer.info.radio_stats.snr_packet) : -1;
    item.icon = online ? nullptr : offline_icon;
    return peer_idx;
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
