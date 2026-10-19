#include "ui.h"

#include "App.h"

#include <pika/lcd/images/pika_logo.h>
#include <pika/log.h>

#include "chprintf.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pika::app {
namespace {

// A cross.
constexpr uint8_t offline_icon[8] = {0x00, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0x00};

// One step up or down a table from current, which need not be in it: from the
// step at or below it, stopping at the ends.
int next_step(std::span<const int> steps, int current, int dir) {
    const int last = (int) steps.size() - 1;
    int i = 0;
    while (i < last and steps[i + 1] <= current) { i++; }
    return steps[std::clamp(i + dir, 0, last)];
}

// LoRa SNR in dB to bars, at least one while online. SF7, the firmware's,
// demodulates down to about -7.5dB, so a single bar is close to that edge.
int signal_bars(float snr, float min_snr) {
    // SNR compensated by SF
    float a = snr - min_snr;
    if (a >= 15.0f) {
        return 4;
    }
    if (a >= 10.0f) {
        return 3;
    }
    if (a >= 5.0f) {
        return 2;
    }
    if (a >= 0.0f) {
        return 1;
    }
    return 0;
}

// A speaker with a sound wave, for the message playing.
constexpr uint8_t speaker_icon[8] = {0x10, 0x34, 0xF2, 0xF2, 0xF2, 0x34, 0x10, 0x00};

// Nothing, to keep a mark's place.
constexpr uint8_t blank_icon[8] = {};

// A tick, for a message delivered or received in full.
constexpr uint8_t done_icon[8] = {0x00, 0x01, 0x03, 0x86, 0xCC, 0x78, 0x30, 0x00};

// Seconds into the day for a timestamp. There is no wall clock yet, so this is
// time since boot; a GNSS or RTC time would go in here.
uint32_t time_of_day_s(Time t) { return (uint32_t) (t / SECOND) % 86400U; }

// A message's length in seconds, rounded, from its size in codec frames.
uint32_t message_seconds(uint32_t size) {
    using audio::MELPeRecorder;
    const uint32_t samples = size / MELPeRecorder::frame_bytes * MELPeRecorder::frame_len;
    return (samples + 4000U) / 8000U;
}

}// namespace

void StatusBar::draw() {
    app_->canvas().rect<false>(0, 0, 160, status_bar_h);
    app_->canvas().hline<true>(0, status_bar_h - 1, 160);
    char buf[20];
    chsnprintf(buf, sizeof(buf), "%s %.2f", app_->battery_status().charging != Charge::off ? "CHG" : "   ",
               app_->battery_status().voltage / 1000.0f);
    app_->canvas().text<true>(2, 0, buf);

    if (app_->locked()) {
        // A padlock in the middle, 7x8: the shackle over a body with a keyhole.
        constexpr int x = 80 - 3;
        app_->canvas().rect<true>(x + 1, 0, 5, 1);
        app_->canvas().rect<true>(x + 1, 1, 1, 2);
        app_->canvas().rect<true>(x + 5, 1, 1, 2);
        app_->canvas().rect<true>(x, 3, 7, 5);
        app_->canvas().rect<false>(x + 3, 4, 1, 2);
    }

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

// Light, set off from the rows below by a line in the gap before them.
void Menu::draw_title(App &app, const char *title) {
    app.canvas().rect<false>(0, title_y, 160, title_h);
    app.canvas().text<true>(2, title_y + 2, title);
    app.canvas().hline<true>(0, title_y + title_h, 160);
}

void Menu::draw() {
    if (title_ != nullptr) {
        draw_title(*app_, title_);
    }

    for (size_t i = 0; i < items_.size(); i++) { draw_item(i); }
    draw_cursor(true);
}

void Menu::draw_item(size_t i) {
    const int y = (int) i * row_h + top_;
    app_->canvas().rect<false>(label_x, y, 160 - label_x, row_h);
    app_->canvas().text(label_x, y, items_[i].name);
    // The value, mark and slot go over the name, so a long one ends where they
    // start. Without bars or an icon there is no slot and the value takes
    // the right edge.
    const bool slot = items_[i].bars >= 0 or items_[i].icon != nullptr;
    int right = slot ? slot_x - 2 : 160;
    if (items_[i].mark != nullptr) {
        right -= icon_size;
        app_->canvas().rect<false>(right - 2, y, icon_size + 2, icon_size);
        app_->canvas().bitmap(right, y, icon_size, icon_size, items_[i].mark);
        right -= 2;
    }
    if (items_[i].value != nullptr) {
        const int w = 8 * (int) strlen(items_[i].value);
        const int x = right - w;
        app_->canvas().rect<false>(x - 2, y, right - x + 2, icon_size);
        app_->canvas().text(x, y, items_[i].value);
    }
    if (slot) {
        draw_slot(*app_, y, items_[i].bars, items_[i].icon);
    }
}

// Bars rising 3, 5, 7 and 9px, bottom aligned one pixel above the next row;
// an unlit bar is its baseline, so all four places show.
void Menu::draw_slot(App &app, int y, int bars, const uint8_t *icon) {
    app.canvas().rect<false>(slot_x, y, slot_w, row_h);
    if (bars >= 0) {
        for (int bar = 0; bar < 4; bar++) {
            const int h = bar < bars ? 3 + 2 * bar : 1;
            app.canvas().rect<true>(slot_x + 3 * bar, y + row_h - 1 - h, 2, h);
        }
    } else if (icon != nullptr) {
        app.canvas().bitmap(slot_x + (slot_w - icon_size) / 2, y, icon_size, icon_size, icon);
    }
}

// As tall as a capital: rows 1, 2, 3, 4, 3, 2, 1 pixels wide.
void Menu::draw_marker(App &app, int y, bool on) {
    if (not on) {
        app.canvas().rect<false>(0, y, 4, 7);
        return;
    }
    for (int row = 0; row < 7; row++) { app.canvas().rect<true>(0, y + row, 4 - std::abs(row - 3), 1); }
}

void Menu::draw_cursor(bool on) {
    if (not items_.empty()) {
        draw_marker(*app_, cursor_ * row_h + top_, on);
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

void SettingsScreen::init() {
    menu_.reset();
    adjusting_ = false;
    show_value(backlight_item);
    show_value(tx_power_item);
}

void SettingsScreen::draw() { menu_.draw(); }

void SettingsScreen::on_press(input::Button button) {
    if (not adjusting_) {
        if (not menu_.on_press(button)) {
            handle_back(button);
        }
        return;
    }
    const size_t item = menu_.cursor();
    switch (button) {
        case input::Button::up:
            adjust(+1);
            break;
        case input::Button::down:
            adjust(-1);
            break;
        case input::Button::left:
        case input::Button::right:
            adjusting_ = false;
            show_value(item);
            app_->save_settings();
            break;
        default:
            return;
    }
    menu_.draw_item(item);
}

void SettingsScreen::start_adjust() {
    adjusting_ = true;
    show_value(menu_.cursor());
    menu_.draw_item(menu_.cursor());
}

void SettingsScreen::adjust(int dir) {
    const size_t item = menu_.cursor();
    switch (item) {
        case backlight_item:
            app_->set_backlight(next_step(backlight_steps, app_->get_backlight(), dir));
            break;
        case tx_power_item: {
            const int current = (int) lroundf(app_->mac().tx_power());
            app_->mac().set_tx_power((float) next_step(tx_power_steps, current, dir));
            break;
        }
        default:
            break;
    }
    show_value(item);
}

void SettingsScreen::show_value(size_t item) {
    char text[8];
    switch (item) {
        case backlight_item:
            chsnprintf(text, sizeof(text), "%d%%", app_->get_backlight() / 10);
            break;
        case tx_power_item:
            chsnprintf(text, sizeof(text), "%d dBm", (int) lroundf(app_->mac().tx_power()));
            break;
        default:
            return;
    }
    const bool adjusted = adjusting_ and item == (size_t) menu_.cursor();
    chsnprintf(values_[item].data(), values_[item].size(), adjusted ? "[%s]" : "%s", text);
    menu_items[item].value = values_[item].data();
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
        peer_items_[i].on_activate = [](void *self) { ((HomeScreen *) self)->open_chat(); };
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

void HomeScreen::open_chat() {
    if (not menu_.empty()) {
        chat_screen_.show(app_->peers()[menu_.cursor()]);
    }
}

void HomeScreen::on_peer_updated(const Peer &peer) {
    chat_screen_.on_peer_updated(peer);
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
        chsnprintf(status.data(), status.size(), "%d %d %c%c", (int) peer.info.radio_stats.rssi_packet,
                   (int) peer.info.radio_stats.rssi_floor, tx, rx);
    } else {
        chsnprintf(status.data(), status.size(), "%c%c", tx, rx);
    }
    item.value = status.data();
    item.bars = online ? (int8_t) signal_bars(peer.info.radio_stats.snr_packet, app_->mac().get_min_snr()) : -1;
    item.icon = online ? nullptr : offline_icon;
    item.mark = app_->is_playing(peer) ? speaker_icon : blank_icon;
    return peer_idx;
}

void HomeScreen::record_message_start() {
    if (not menu_.empty()) {
        chat_screen_.record(app_->peers()[menu_.cursor()]);
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

void ChatScreen::show(const Peer &peer) {
    peer_ = &peer;
    first_ = 0;
    follow_ = true;
    app_->activate_screen(*this);
}

void ChatScreen::draw() {
    Menu::draw_title(*app_, peer_->name.data());
    // The peer's signal at the title's right end, as on the home screen, its
    // bars down to the pixel above the title's line.
    const PeerInfo &info = peer_->info;
    Menu::draw_slot(*app_, Menu::title_y + 2,
                    info.is_online ? signal_bars(info.radio_stats.snr_packet, app_->mac().get_min_snr()) : -1,
                    info.is_online ? nullptr : offline_icon);

    size_t n = 0;
    for (const TxMessage &msg: peer_->tx_msgs) { lines_[n++] = {msg.created, &msg, nullptr}; }
    for (const RxMessage &msg: peer_->rx_msgs) { lines_[n++] = {msg.created, nullptr, &msg}; }
    std::sort(lines_.begin(), lines_.begin() + n, [](const Line &a, const Line &b) { return a.created < b.created; });
    lines_n_ = (int) n;

    const int last = std::max(0, lines_n_ - 1);
    cursor_ = follow_ ? last : std::clamp(cursor_, 0, last);
    follow_ = cursor_ == last;
    // Scrolled just enough to keep the cursor in view.
    first_ = std::clamp(first_, std::max(0, cursor_ - rows + 1), cursor_);

    app_->canvas().rect<false>(0, Menu::items_y, 160, rows * Menu::row_h);
    for (int row = 0; row < rows and first_ + row < lines_n_; row++) { draw_line(row, lines_[first_ + row]); }
    if (lines_n_ > 0) {
        Menu::draw_marker(*app_, Menu::items_y + (cursor_ - first_) * Menu::row_h, true);
    }

    // The scroll bar: a 1px track, and a thumb as long as the share of the
    // lines in view, as far down it as they are; the whole track if all fit.
    constexpr int track_h = rows * Menu::row_h - 1;
    const int total = std::max(lines_n_, 1);
    const int shown = std::min(rows, total);
    const int thumb_h = std::max(3, track_h * shown / total);
    const int thumb_y = total > shown ? (track_h - thumb_h) * first_ / (total - shown) : 0;
    app_->canvas().vline<true>(scroll_x + 1, Menu::items_y, track_h);
    app_->canvas().rect<true>(scroll_x, Menu::items_y + thumb_y, scroll_w, thumb_h);
}

// "> T   12:55:23     25s [HHH  ]", R one column in for a received message,
// the cursor's place at the left edge, the length right-aligned to the bar.
void ChatScreen::draw_line(int row, const Line &line) {
    auto &canvas = app_->canvas();
    const int y = Menu::items_y + row * Menu::row_h;
    const bool tx = line.tx != nullptr;
    canvas.text(tx ? 6 : 14, y, tx ? "T" : "R");

    char text[12];
    const uint32_t s = time_of_day_s(line.created);
    chsnprintf(text, sizeof(text), "%02u:%02u:%02u", (unsigned) (s / 3600U), (unsigned) (s / 60U % 60U),
               (unsigned) (s % 60U));
    canvas.text(30, y, text);

    // A tick once done, else a bar of the bytes through.

    const uint32_t size = tx ? line.tx->size : line.rx->size;
    const int len = chsnprintf(text, sizeof(text), "%us", (unsigned) message_seconds(size));
    // The speaker while this message plays, in a place kept for it.
    constexpr int mark_x = bar_x - 2 - 8;
    canvas.text(mark_x - 2 - 8 * len, y, text);
    if (app_->is_playing(*peer_, tx, tx ? line.tx->id : line.rx->id)) {
        canvas.bitmap(mark_x, y, 8, 8, speaker_icon);
    }
    const bool done = tx ? (line.tx->flags & TxMessage::acked) != 0 : size > 0 and line.rx->size_received >= size;
    if (done) {
        canvas.bitmap(bar_x + 2, y, 8, 8, done_icon);
        return;
    }
    const uint32_t through = std::min(tx ? line.tx->size_sent : line.rx->size_received, size);
    canvas.frame<true>(bar_x, y, 12, 7);
    if (size > 0) {
        canvas.rect<true>(bar_x + 1, y + 1, (int) (10U * through / size), 5);
    }
}

void ChatScreen::on_press(input::Button button) {
    switch (button) {
        case input::Button::up:
            if (cursor_ > 0) {
                cursor_--;
                follow_ = false;
                draw();
            }
            break;
        case input::Button::down:
            cursor_++;
            draw();
            break;
        case input::Button::ptt:
            record(*peer_);
            break;
        case input::Button::lsn:
            play_selected();
            break;
        default:
            handle_back(button);
            break;
    }
}

// A received message plays once it is all here; an own one always can.
void ChatScreen::play_selected() {
    if (lines_n_ == 0) {
        return;
    }
    const Line &line = lines_[cursor_];
    if (line.tx != nullptr) {
        app_->play_message(*peer_, true, line.tx->id);
    } else if (line.rx->size > 0 and line.rx->size_received >= line.rx->size) {
        app_->play_message(*peer_, false, line.rx->id);
    }
}

void ChatScreen::record(const Peer &peer) {
    // The release ends it, on the recording screen, which comes back here.
    if (app_->start_recording(&peer - app_->peers().data())) {
        peer_ = &peer;
        // Back here, the cursor lands on it: the newest, so the last line.
        follow_ = true;
        recording_screen_.show(peer);
    }
}

void ChatScreen::on_peer_updated(const Peer &peer) {
    if (&peer == peer_ and app_->is_active(*this)) {
        draw();
    }
}

}// namespace pika::app
