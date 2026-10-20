#pragma once

#include "Peer.h"

#include <pika/buttons.h>
#include <pika/lcd/ST75160.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace pika::app {

class App;

// Where things go on the panel, in pixels. Text is the 8x8 font, set in rows
// under the status bar and, on most screens, a title.
namespace layout {
inline constexpr int width = lcd::ST75160::Framebuffer::width;
inline constexpr int height = lcd::ST75160::Framebuffer::height;
inline constexpr int glyph = 8;// The font's cell, as wide as it is high.
inline constexpr int icon = 8; // Bitmap icons are square.
inline constexpr int gap = 2;  // Between things on a line, and from the panel's edges.

// The status bar, its line on its last row.
inline constexpr int status_h = 10;
// The title, its line on the row after it.
inline constexpr int title_y = status_h - 1;
inline constexpr int title_h = 12;
// Rows: under the title's line and a pixel of space, or under the status bar
// where there is no title.
inline constexpr int rows_y = title_y + title_h + 2;
inline constexpr int rows_y_untitled = status_h + 1;
inline constexpr int row_h = 10;

// The cursor triangle at the left edge, and row text clear of it.
inline constexpr int marker_w = 4;
inline constexpr int label_x = 8;

// The slot at a row's right end for signal bars: four 2px bars with 1px
// between, or an icon in the middle.
inline constexpr int slot_w = 4 * 2 + 3;
inline constexpr int slot_x = width - slot_w;
}// namespace layout

// The status bar across the top: battery, the keyboard lock, the radio.
void draw_status_bar(App &app);

// A list with a cursor, for a screen to draw its rows into. It keeps the
// cursor on a line and in view, scrolling as it moves, and can follow the last
// line as lines are added. It draws the cursor and, if asked, a scroll bar
// down the right edge; the rows are the screen's.
class ListView {
public:
    static constexpr int scroll_w = 3;
    static constexpr int scroll_x = layout::width - scroll_w;

    // The first row at top.
    ListView(int top, bool scroll_bar) :
            top_(top), rows_((layout::height - top) / layout::row_h), scroll_bar_(scroll_bar) {}

    // To the first line, or onto the last and following it.
    void reset() {
        cursor_ = 0;
        first_ = 0;
        follow_ = false;
    }

    void follow() { follow_ = true; }

    // How many lines there are now; called before drawing them.
    void set_count(int count);

    // UP and DOWN move the cursor; false for any other button.
    bool on_press(input::Button button);

    int cursor() const { return cursor_; }

    bool empty() const { return count_ == 0; }

    // The lines in view are first() up to first() + shown(), each at row_y().
    int first() const { return first_; }

    int shown() const { return std::min(rows_, count_ - first_); }

    int row_y(int line) const { return top_ + (line - first_) * layout::row_h; }

    // The cursor and the scroll bar, over the rows.
    void draw(App &app) const;

private:
    const int top_;
    const int rows_;
    const bool scroll_bar_;
    int count_ = 0;
    int cursor_ = 0;
    int first_ = 0;
    bool follow_ = false;// On the last line, so it moves on to a new one.

    // Just enough to keep the cursor in view.
    void scroll();
};

// A screen draws everything below the status bar from current state, every
// pass of App::main(), onto a cleared canvas; it keeps no drawn state.
class Screen {
public:
    virtual ~Screen() = default;

    virtual void draw() = 0;

    virtual void on_press(input::Button button) = 0;

    virtual void on_release(input::Button button) { (void) button; }

    virtual bool show_status_bar() const { return true; }

protected:
    App *app_{};
    Screen *parent_screen_{};

    Screen(App &app, Screen *parent_screen) : app_(&app), parent_screen_(parent_screen) {}

    void back();

    // LEFT goes back; false for any other button.
    bool handle_back(input::Button button);
};

class AboutScreen : public Screen {
public:
    AboutScreen(App &app, Screen *parent_screen) : Screen(app, parent_screen) {}

    void show();

    void draw() override;

    void on_press(input::Button button) override;

    bool show_status_bar() const override { return false; }
};

// RIGHT on a setting starts adjusting it: UP/DOWN step the value, stopping at
// the ends, and LEFT or RIGHT finishes and saves. The value being adjusted
// shows in brackets. The volume, the one most often changed, is on the main
// menu instead, adjusted the same way.
class SettingsScreen : public Screen {
public:
    SettingsScreen(App &app, Screen *parent_screen) : Screen(app, parent_screen) {}

    void show();

    void draw() override;

    void on_press(input::Button button) override;

private:
    // PWM duty out of 1000: 0, 10, 20, 50 and 100%.
    static constexpr int backlight_steps[] = {0, 100, 200, 500, 1000};

    // dBm at the antenna. The SX1262's -9..22dBm behind the module's 10dB PA
    // reaches 1..32; the driver clamps the rest.
    static constexpr int tx_power_steps[] = {1, 10, 14, 17, 20, 24, 27, 30};

    enum { backlight_item, tx_power_item, items_num };

    static constexpr const char *names[items_num] = {"Backlight", "TX power"};

    ListView list_{layout::rows_y, false};
    bool adjusting_ = false;

    // One step, +1 up or -1 down, of the setting under the cursor.
    void adjust(int dir);
};

class MainMenuScreen : public Screen {
public:
    MainMenuScreen(App &app, Screen *parent_screen) : Screen(app, parent_screen) {}

    void show();

    void draw() override;

    void on_press(input::Button button) override;

private:
    enum { volume_item, settings_item, about_item, items_num };

    static constexpr const char *names[items_num] = {"Volume", "Settings", "About"};

    AboutScreen about_screen_{*app_, this};
    SettingsScreen settings_screen_{*app_, this};
    ListView list_{layout::rows_y, false};
    bool adjusting_ = false;// The volume.
};

// Shown while a message is recorded; releasing PTT finishes it and goes back.
class RecordingScreen : public Screen {
public:
    RecordingScreen(App &app, Screen *parent_screen) : Screen(app, parent_screen) {}

    void show(const Peer &peer);

    void draw() override;

    void on_press(input::Button button) override { (void) button; }

    void on_release(input::Button button) override;

private:
    const Peer *peer_ = nullptr;
};

// The messages with one peer as a chat, oldest at the top and new ones added
// at the bottom: T or R in a column of its own, the time, the length, and a
// progress bar until it is done. UP/DOWN move the cursor, LSN plays the
// message under it, PTT records a new one to this peer, LEFT goes back.
class ChatScreen : public Screen {
public:
    ChatScreen(App &app, Screen *parent_screen) : Screen(app, parent_screen) {}

    void show(Peer &peer);

    // Records a new message to peer; releasing PTT comes back to its chat,
    // with the cursor on the new message.
    void record(Peer &peer);

    void draw() override;

    void on_press(input::Button button) override;

private:
    // Right to left from the scroll bar: a progress bar or tick, then the
    // speaker while a message plays.
    static constexpr int progress_w = 12;
    static constexpr int progress_x = ListView::scroll_x - layout::gap - progress_w;
    static constexpr int mark_x = progress_x - layout::gap - layout::icon;
    // T and R in columns of their own, the time one column clear of them.
    static constexpr int tx_x = layout::marker_w + layout::gap;
    static constexpr int rx_x = tx_x + layout::glyph;
    static constexpr int time_x = rx_x + 2 * layout::glyph;

    // A sent or a received message.
    struct Line {
        Time created;
        const TxMessage *tx;
        const RxMessage *rx;
    };

    RecordingScreen recording_screen_{*app_, this};
    Peer *peer_ = nullptr;
    ListView list_{layout::rows_y, true};
    // Both rings, merged in time order; a member, too big for the App stack.
    std::array<Line, 2 * MSGS_MAX_NUM> lines_{};
    int lines_n_ = 0;

    // Rebuilds lines_ from the peer's messages, and tells the list.
    void collect();

    void draw_line(const Line &line, int y);

    void play_selected();
};

// The peers, each with its status, the speaker while one of its messages
// plays, and its signal. RIGHT opens the chat, PTT records to the peer, LSN
// plays its last message, PWR opens the main menu.
class HomeScreen : public Screen {
public:
    explicit HomeScreen(App &app) : Screen(app, nullptr) {}

    void draw() override;

    void on_press(input::Button button) override;

private:
    MainMenuScreen main_menu_screen_{*app_, this};
    ChatScreen chat_screen_{*app_, this};
    ListView list_{layout::rows_y_untitled, false};

    // The peer under the cursor; null if there are none.
    Peer *selected();

    void draw_row(const Peer &peer, int y);
};

}// namespace pika::app
