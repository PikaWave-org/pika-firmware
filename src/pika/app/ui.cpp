#include "ui.h"

#include "App.h"

#include <pika/lcd/images/pika_logo.h>

#include "chprintf.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>

namespace pika::app {
namespace {

// A cross, for a peer offline.
constexpr uint8_t offline_icon[8] = {0x00, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0x00};

// A speaker with a sound wave, for the message playing.
constexpr uint8_t speaker_icon[8] = {0x10, 0x34, 0xF2, 0xF2, 0xF2, 0x34, 0x10, 0x00};

// A tick, for a message delivered or received in full.
constexpr uint8_t done_icon[8] = {0x00, 0x01, 0x03, 0x86, 0xCC, 0x78, 0x30, 0x00};

// One step up or down a table from current, which need not be in it: from the
// step at or below it, stopping at the ends.
int next_step(std::span<const int> steps, int current, int dir) {
    const int last = (int) steps.size() - 1;
    int i = 0;
    while (i < last and steps[i + 1] <= current) { i++; }
    return steps[std::clamp(i + dir, 0, last)];
}

// Volume in dB of full scale: muted, which silences beeps too, then 3dB steps.
constexpr int volume_steps[] = {App::volume_muted_db, -30, -27, -24, -21, -18, -15, -12, -9, -6, -3, 0};

// LoRa SNR in dB to bars: none below min_snr, the radio's floor at its SF,
// then one more for every 5dB above it, up to 4.
int signal_bars(float snr, float min_snr) {
    const float above = snr - min_snr;
    if (above >= 15.0f) {
        return 4;
    }
    if (above >= 10.0f) {
        return 3;
    }
    if (above >= 5.0f) {
        return 2;
    }
    if (above >= 0.0f) {
        return 1;
    }
    return 0;
}

// Seconds into the day for a timestamp. There is no wall clock yet, so this is
// time since boot; a GNSS or RTC time would go in here.
uint32_t time_of_day_s(Time t) { return (uint32_t) (t / SECOND) % 86400U; }

// A message's length in seconds, rounded, from its size in codec frames.
uint32_t message_seconds(uint32_t size) {
    using audio::MELPeRecorder;
    const uint32_t samples = size / MELPeRecorder::frame_bytes * MELPeRecorder::frame_len;
    return (samples + 4000U) / 8000U;
}

void draw_icon(App &app, int x, int y, const uint8_t *icon) {
    app.canvas().bitmap(x, y, layout::icon, layout::icon, icon);
}

// Light, set off from the rows below by a line in the gap before them.
void draw_title(App &app, const char *title) {
    app.canvas().text<true>(layout::gap, layout::title_y + layout::gap, title);
    app.canvas().hline<true>(0, layout::title_y + layout::title_h, layout::width);
}

// The cursor: a filled triangle at the left edge, as tall as a capital, rows
// 1, 2, 3, 4, 3, 2, 1 pixels wide.
void draw_marker(App &app, int y) {
    for (int row = 0; row < 7; row++) { app.canvas().rect<true>(0, y + row, layout::marker_w - std::abs(row - 3), 1); }
}

// The peer's signal in the slot at the right end of the row at y, cleared
// first: bars rising 3, 5, 7 and 9px, bottom aligned one pixel above the next
// row, an unlit one its baseline so all four places show; the cross offline.
void draw_signal(App &app, int y, const PeerInfo &info) {
    app.canvas().rect<false>(layout::slot_x, y, layout::slot_w, layout::row_h);
    if (not info.is_online) {
        draw_icon(app, layout::slot_x + (layout::slot_w - layout::icon) / 2, y, offline_icon);
        return;
    }
    const int bars = signal_bars(info.radio_stats.snr_packet, app.mac().get_min_snr());
    for (int bar = 0; bar < 4; bar++) {
        const int h = bar < bars ? 3 + 2 * bar : 1;
        app.canvas().rect<true>(layout::slot_x + 3 * bar, y + layout::row_h - 1 - h, 2, h);
    }
}

// Adjusting a setting, the same in every menu: UP/DOWN step it, through
// step(+1 or -1), and LEFT or RIGHT finishes and saves it.
template<class Step>
void adjust_press(App &app, bool &adjusting, input::Button button, Step step) {
    switch (button) {
        case input::Button::up:
            step(+1);
            break;
        case input::Button::down:
            step(-1);
            break;
        case input::Button::left:
        case input::Button::right:
            adjusting = false;
            app.save_settings();
            break;
        default:
            break;
    }
}

// A menu row: the name, and any value right-aligned over it.
void draw_label_row(App &app, int y, const char *name, const char *value) {
    app.canvas().text(layout::label_x, y, name);
    if (value != nullptr) {
        const int x = layout::width - layout::glyph * (int) strlen(value);
        app.canvas().rect<false>(x - layout::gap, y, layout::width - x + layout::gap, layout::glyph);
        app.canvas().text(x, y, value);
    }
}

// A setting's row: its value in brackets while it is being adjusted.
void draw_setting_row(App &app, int y, const char *name, const char *value, bool adjusting) {
    char text[16];
    chsnprintf(text, sizeof(text), adjusting ? "[%s]" : "%s", value);
    draw_label_row(app, y, name, text);
}

}// namespace

void draw_status_bar(App &app) {
    auto &canvas = app.canvas();
    canvas.hline<true>(0, layout::status_h - 1, layout::width);
    char buf[20];
    chsnprintf(buf, sizeof(buf), "%s %.2f", app.battery_status().charging != Charge::off ? "CHG" : "   ",
               app.battery_status().voltage / 1000.0f);
    canvas.text<true>(layout::gap, 0, buf);

    if (app.locked()) {
        // A padlock in the middle, 7x8: the shackle over a body with a keyhole.
        constexpr int x = layout::width / 2 - 3;
        canvas.rect<true>(x + 1, 0, 5, 1);
        canvas.rect<true>(x + 1, 1, 1, 2);
        canvas.rect<true>(x + 5, 1, 1, 2);
        canvas.rect<true>(x, 3, 7, 5);
        canvas.rect<false>(x + 3, 4, 1, 2);
    }

    // R, or T one column on, at the right end.
    constexpr int radio_x = layout::width - layout::gap - 2 * layout::glyph;
    switch (app.mac().tx_state()) {
        case RadioState::tx:
            canvas.text<true>(radio_x + layout::glyph, 0, "T");
            break;
        case RadioState::rx:
            canvas.text<true>(radio_x, 0, "R");
            break;
        default:
            break;
    }
}

void ListView::set_count(int count) {
    count_ = count;
    const int last = std::max(0, count_ - 1);
    cursor_ = follow_ ? last : std::clamp(cursor_, 0, last);
    follow_ = cursor_ == last;
    scroll();
}

bool ListView::on_press(input::Button button) {
    switch (button) {
        case input::Button::up:
            cursor_ = std::max(0, cursor_ - 1);
            break;
        case input::Button::down:
            cursor_ = std::min(std::max(0, count_ - 1), cursor_ + 1);
            break;
        default:
            return false;
    }
    follow_ = cursor_ == std::max(0, count_ - 1);
    scroll();
    return true;
}

void ListView::scroll() { first_ = std::clamp(first_, std::max(0, cursor_ - rows_ + 1), cursor_); }

void ListView::draw(App &app) const {
    if (count_ > 0) {
        draw_marker(app, row_y(cursor_));
    }
    if (not scroll_bar_) {
        return;
    }
    // A 1px track, and a thumb as long as the share of the lines in view, as
    // far down it as they are; the whole track if all fit.
    const int track_h = rows_ * layout::row_h - 1;
    const int total = std::max(count_, 1);
    const int in_view = std::min(rows_, total);
    const int thumb_h = std::max(3, track_h * in_view / total);
    const int thumb_y = total > in_view ? (track_h - thumb_h) * first_ / (total - in_view) : 0;
    app.canvas().vline<true>(scroll_x + 1, top_, track_h);
    app.canvas().rect<true>(scroll_x, top_ + thumb_y, scroll_w, thumb_h);
}

void Screen::back() { app_->show(*parent_screen_); }

bool Screen::handle_back(input::Button button) {
    if (button != input::Button::left or parent_screen_ == nullptr) {
        return false;
    }
    back();
    return true;
}

void AboutScreen::show() { app_->show(*this); }

void AboutScreen::draw() {
    app_->canvas().bitmap(0, 0, pika::lcd::pika_logo_width, pika::lcd::pika_logo_height, pika::lcd::pika_logo);
    app_->canvas().text(layout::width / 2, layout::height - layout::glyph, BOARD_NAME);
}

// Any button goes back.
void AboutScreen::on_press(input::Button) { back(); }

void SettingsScreen::show() {
    list_.reset();
    adjusting_ = false;
    app_->show(*this);
}

void SettingsScreen::draw() {
    draw_title(*app_, "Settings");
    list_.set_count(items_num);
    for (int i = list_.first(); i < list_.first() + list_.shown(); i++) {
        char value[12];
        if (i == backlight_item) {
            chsnprintf(value, sizeof(value), "%d%%", app_->get_backlight() / 10);
        } else {
            chsnprintf(value, sizeof(value), "%d dBm", (int) lroundf(app_->mac().tx_power()));
        }
        draw_setting_row(*app_, list_.row_y(i), names[i], value, adjusting_ and i == list_.cursor());
    }
    list_.draw(*app_);
}

void SettingsScreen::on_press(input::Button button) {
    if (not adjusting_) {
        if (button == input::Button::right) {
            adjusting_ = true;
        } else if (not list_.on_press(button)) {
            handle_back(button);
        }
        return;
    }
    adjust_press(*app_, adjusting_, button, [this](int dir) { adjust(dir); });
}

void SettingsScreen::adjust(int dir) {
    if (list_.cursor() == backlight_item) {
        app_->set_backlight(next_step(backlight_steps, app_->get_backlight(), dir));
    } else {
        app_->mac().set_tx_power((float) next_step(tx_power_steps, (int) lroundf(app_->mac().tx_power()), dir));
    }
}

void MainMenuScreen::show() {
    list_.reset();
    adjusting_ = false;
    app_->show(*this);
}

void MainMenuScreen::draw() {
    draw_title(*app_, "Main Menu");
    list_.set_count(items_num);
    for (int i = list_.first(); i < list_.first() + list_.shown(); i++) {
        if (i == volume_item) {
            char value[8];
            if (app_->volume_db() <= App::volume_muted_db) {
                strcpy(value, "muted");
            } else {
                chsnprintf(value, sizeof(value), "%d dB", app_->volume_db());
            }
            draw_setting_row(*app_, list_.row_y(i), names[i], value, adjusting_);
        } else {
            draw_label_row(*app_, list_.row_y(i), names[i], nullptr);
        }
    }
    list_.draw(*app_);
}

void MainMenuScreen::on_press(input::Button button) {
    if (adjusting_) {
        adjust_press(*app_, adjusting_, button, [this](int dir) {
            // A test tone for each change, none at the ends.
            const int db = next_step(volume_steps, app_->volume_db(), dir);
            if (db != app_->volume_db()) {
                app_->set_volume_db(db);
                app_->beep_volume();
            }
        });
    } else if (button == input::Button::right) {
        switch (list_.cursor()) {
            case volume_item:
                adjusting_ = true;
                break;
            case settings_item:
                settings_screen_.show();
                break;
            default:
                about_screen_.show();
                break;
        }
    } else if (not list_.on_press(button)) {
        handle_back(button);
    }
}

void RecordingScreen::show(const Peer &peer) {
    peer_ = &peer;
    app_->show(*this);
}

void RecordingScreen::draw() {
    app_->canvas().text(2 * layout::gap, 3 * layout::row_h, "Recording message:");
    app_->canvas().text(2 * layout::gap, 5 * layout::row_h, peer_->name.data());
}

void RecordingScreen::on_release(input::Button button) {
    if (button == input::Button::ptt) {
        app_->finish_recording();
        back();
    }
}

void ChatScreen::show(Peer &peer) {
    peer_ = &peer;
    list_.reset();
    list_.follow();
    app_->show(*this);
}

void ChatScreen::record(Peer &peer) {
    // The release ends it, on the recording screen, which comes back here.
    if (app_->start_recording(peer)) {
        peer_ = &peer;
        // Back here, the cursor lands on it: the newest, so the last line.
        list_.follow();
        recording_screen_.show(peer);
    }
}

void ChatScreen::collect() {
    size_t n = 0;
    for (const TxMessage &msg: peer_->tx_msgs) { lines_[n++] = {msg.created, &msg, nullptr}; }
    for (const RxMessage &msg: peer_->rx_msgs) { lines_[n++] = {msg.created, nullptr, &msg}; }
    std::sort(lines_.begin(), lines_.begin() + n, [](const Line &a, const Line &b) { return a.created < b.created; });
    lines_n_ = (int) n;
    list_.set_count(lines_n_);
}

void ChatScreen::draw() {
    draw_title(*app_, peer_->name.data());
    // The peer's signal at the title's right end, as on the home screen.
    draw_signal(*app_, layout::title_y + layout::gap, peer_->info);

    collect();
    for (int i = list_.first(); i < list_.first() + list_.shown(); i++) { draw_line(lines_[i], list_.row_y(i)); }
    list_.draw(*app_);
}

// "> T   12:55:23     25s [HHH  ]", R one column in for a received message,
// the cursor's place at the left edge, the length right-aligned to the bar.
void ChatScreen::draw_line(const Line &line, int y) {
    auto &canvas = app_->canvas();
    const bool tx = line.tx != nullptr;
    canvas.text(tx ? tx_x : rx_x, y, tx ? "T" : "R");

    char text[12];
    const uint32_t s = time_of_day_s(line.created);
    chsnprintf(text, sizeof(text), "%02u:%02u:%02u", (unsigned) (s / 3600U), (unsigned) (s / 60U % 60U),
               (unsigned) (s % 60U));
    canvas.text(time_x, y, text);

    const uint32_t size = tx ? line.tx->size : line.rx->size;
    const int len = chsnprintf(text, sizeof(text), "%us", (unsigned) message_seconds(size));
    canvas.text(mark_x - layout::gap - layout::glyph * len, y, text);
    // The speaker while this message plays, in a place kept for it.
    if (app_->is_playing(*peer_, tx, tx ? line.tx->id : line.rx->id)) {
        draw_icon(*app_, mark_x, y, speaker_icon);
    }

    // A tick once done, else a bar of the bytes through.
    if (tx ? line.tx->delivered() : line.rx->complete()) {
        draw_icon(*app_, progress_x + (progress_w - layout::icon) / 2, y, done_icon);
        return;
    }
    const uint32_t through = std::min(tx ? line.tx->size_sent : line.rx->size_received, size);
    canvas.frame<true>(progress_x, y, progress_w, layout::glyph - 1);
    if (size > 0) {
        canvas.rect<true>(progress_x + 1, y + 1, (int) ((progress_w - 2) * through / size), layout::glyph - 3);
    }
}

void ChatScreen::on_press(input::Button button) {
    switch (button) {
        case input::Button::ptt:
            record(*peer_);
            break;
        case input::Button::lsn:
            play_selected();
            break;
        default:
            if (not list_.on_press(button)) {
                handle_back(button);
            }
            break;
    }
}

// A received message plays once it is all here; an own one always can.
void ChatScreen::play_selected() {
    collect();
    if (list_.empty()) {
        return;
    }
    const Line &line = lines_[list_.cursor()];
    if (line.tx != nullptr) {
        app_->play_message(*peer_, true, line.tx->id);
    } else if (line.rx->complete()) {
        app_->play_message(*peer_, false, line.rx->id);
    }
}

Peer *HomeScreen::selected() { return list_.empty() ? nullptr : &app_->peers()[list_.cursor()]; }

void HomeScreen::draw() {
    const auto peers = app_->peers();
    list_.set_count((int) peers.size());
    for (int i = list_.first(); i < list_.first() + list_.shown(); i++) { draw_row(peers[i], list_.row_y(i)); }
    list_.draw(*app_);
}

// The name, then over its end the status, the speaker's place and the signal.
void HomeScreen::draw_row(const Peer &peer, int y) {
    auto &canvas = app_->canvas();
    canvas.text(layout::label_x, y, peer.name.data());

    const bool tx_pending = std::any_of(peer.tx_msgs.begin(), peer.tx_msgs.end(),
                                        [](const TxMessage &msg) { return not msg.delivered(); });
    const bool rx_pending =
            std::any_of(peer.rx_msgs.begin(), peer.rx_msgs.end(), [](const RxMessage &msg) { return msg.pending(); });
    const char tx = tx_pending ? 'T' : ' ';
    const char rx = rx_pending ? 'R' : ' ';
    char status[12];
    int len;
    if (peer.info.is_online) {
        len = chsnprintf(status, sizeof(status), "%d %d %c%c", (int) peer.info.radio_stats.rssi_packet,
                         (int) peer.info.radio_stats.rssi_floor, tx, rx);
    } else {
        len = chsnprintf(status, sizeof(status), "%c%c", tx, rx);
    }

    // The speaker's place is kept, so the status does not move when it shows.
    constexpr int mark_x = layout::slot_x - layout::gap - layout::icon;
    const int right = mark_x - layout::gap;
    const int x = right - layout::glyph * len;
    canvas.rect<false>(x - layout::gap, y, right - x + layout::gap, layout::glyph);
    canvas.text(x, y, status);
    canvas.rect<false>(mark_x - layout::gap, y, layout::icon + layout::gap, layout::icon);
    if (app_->is_playing(peer)) {
        draw_icon(*app_, mark_x, y, speaker_icon);
    }
    draw_signal(*app_, y, peer.info);
}

void HomeScreen::on_press(input::Button button) {
    Peer *peer = selected();
    switch (button) {
        case input::Button::right:
            if (peer != nullptr) {
                chat_screen_.show(*peer);
            }
            break;
        case input::Button::ptt:
            if (peer != nullptr) {
                chat_screen_.record(*peer);
            }
            break;
        case input::Button::lsn:
            if (peer != nullptr) {
                app_->play_last_message(*peer);
            }
            break;
        case input::Button::pwr:
            main_menu_screen_.show();
            break;
        default:
            list_.on_press(button);
            break;
    }
}

}// namespace pika::app
