#pragma once

#include "Peer.h"

#include <pika/buttons.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace pika::app {

class App;

class StatusBar {
public:
    static constexpr int status_bar_h = 10;

    explicit StatusBar(App &app) : app_(&app) {}

    void draw();

private:
    App *app_{};
};

struct MenuItem {
    const char *name;
    void (*on_activate)(void *screen) = nullptr;
    // At the right end of the row: signal bars, 0..4 of them lit, or else an
    // 8x8 icon; neither if bars < 0 and icon is null.
    int8_t bars = -1;
    const uint8_t *icon = nullptr;
    // Short text right-aligned before them; none if null.
    const char *value = nullptr;
    // 8x8 between the value and the slot; none if null. A blank one keeps the
    // place, so the value does not move when a mark comes and goes.
    const uint8_t *mark = nullptr;
};

// A titled list of items with a cursor; activating an item calls it with owner.
class Menu {
public:
    // A null title leaves the title bar out, and the rows start under the status bar.
    Menu(App &app, const char *title, std::span<const MenuItem> items, void *owner) :
            app_(&app), title_(title), items_(items), owner_(owner),
            top_(title != nullptr ? items_y : StatusBar::status_bar_h + 1) {}

    void set_items(std::span<const MenuItem> items) { items_ = items; }

    void reset() { cursor_ = 0; }

    int cursor() const { return cursor_; }

    bool empty() const { return items_.empty(); }

    void draw();

    // Redraws one row, after its item changed.
    void draw_item(size_t i);

    // Up, down and right; returns false for any other button.
    bool on_press(input::Button button);

    // The layout, for screens drawing their own rows under the same title.
    static constexpr int row_h = 10;
    static constexpr int title_h = 12;
    static constexpr int title_y = StatusBar::status_bar_h - 1;
    // Below the title's line, at title_y + title_h, and a pixel of space.
    static constexpr int items_y = title_y + title_h + 2;

    // The inverted title bar.
    static void draw_title(App &app, const char *title);

    // The cursor: a filled triangle at the left edge of the row at y.
    static void draw_marker(App &app, int y, bool on);

    // The slot at the right end of the row at y, cleared, then signal bars
    // with that many lit if bars >= 0, else the 8x8 icon if any.
    static void draw_slot(App &app, int y, int bars, const uint8_t *icon);

private:
    static constexpr int label_x = 8;
    // The slot at the row's right end: four 2px bars with 1px between.
    static constexpr int slot_w = 4 * 2 + 3;
    static constexpr int slot_x = 160 - slot_w;
    static constexpr int icon_size = 8;

    App *app_;
    const char *title_;
    std::span<const MenuItem> items_;
    void *owner_;
    int top_;// The first row's y.
    int cursor_ = 0;

    void draw_cursor(bool on);


    void move_cursor(int cursor);
};

class Screen {
public:
    void init_activate();

    virtual ~Screen() = default;

    virtual void init() {}

    virtual void draw() = 0;

    virtual void on_press(input::Button button) = 0;

    virtual void on_release(input::Button button) { (void) button; }

    virtual bool show_status_bar() const { return true; }

protected:
    App *app_{};
    Screen *parent_screen_{};

    Screen(App &app, Screen *parent_screen) : app_(&app), parent_screen_(parent_screen) {}

    // Left goes back to the parent screen.
    bool handle_back(input::Button button);
};

class AboutScreen : public Screen {
public:
    AboutScreen(App &app, Screen *parent_screen) : Screen(app, parent_screen) {}

    void draw() override;

    void on_press(input::Button button) override;

    bool show_status_bar() const override { return false; };
};

// RIGHT on a setting starts adjusting it: UP/DOWN step the value, stopping at
// the ends, and LEFT finishes. The value being adjusted shows in brackets.
class SettingsScreen : public Screen {
public:
    SettingsScreen(App &app, Screen *parent_screen) : Screen(app, parent_screen) {}

    void init() override;

    void draw() override;

    void on_press(input::Button button) override;

private:
    // PWM duty out of 1000: 0, 10, 20, 50 and 100%.
    static constexpr int backlight_steps[] = {0, 100, 200, 500, 1000};

    // dBm at the antenna. The SX1262's -9..22dBm behind the module's 10dB PA
    // reaches 1..32; the driver clamps the rest.
    static constexpr int tx_power_steps[] = {1, 10, 14, 17, 20, 24, 27, 30};

    enum : size_t { backlight_item, tx_power_item, items_num };

    std::array<MenuItem, items_num> menu_items{{
            {"Backlight", [](void *self) { ((SettingsScreen *) self)->start_adjust(); }},
            {"TX power", [](void *self) { ((SettingsScreen *) self)->start_adjust(); }},
    }};
    Menu menu_{*app_, "Settings", menu_items, this};
    std::array<std::array<char, 12>, items_num> values_{};
    bool adjusting_ = false;

    void start_adjust();

    // One step, +1 up or -1 down, of the setting under the cursor.
    void adjust(int dir);

    void show_value(size_t item);
};

class MainMenuScreen : public Screen {
public:
    ~MainMenuScreen() = default;

    MainMenuScreen(App &app, Screen *parent_screen) : Screen(app, parent_screen) {}

    void init() override { menu_.reset(); }

    void draw() override;

    void on_press(input::Button button) override;

private:
    AboutScreen about_screen_{*app_, this};
    SettingsScreen settings_screen_{*app_, this};

    std::array<MenuItem, 2> menu_items{{
            {"Settings", [](void *self) { ((MainMenuScreen *) self)->settings_screen_.init_activate(); }},
            {"About", [](void *self) { ((MainMenuScreen *) self)->about_screen_.init_activate(); }},
    }};
    Menu menu_{*app_, "Main Menu", menu_items, this};
};

// Shown while a message is recorded; PTT finishes it.
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

    void show(const Peer &peer);

    // Records a new message to peer; releasing PTT comes back to its chat,
    // with the cursor on the new message.
    void record(const Peer &peer);

    void draw() override;

    void on_press(input::Button button) override;

    // Redraws if this peer's chat is showing.
    void on_peer_updated(const Peer &peer);

private:
    static constexpr int rows = (100 - Menu::items_y) / Menu::row_h;
    // The scroll bar down the right edge, then a progress bar or tick 12px
    // wide two pixels in from it.
    static constexpr int scroll_w = 3;
    static constexpr int scroll_x = 160 - scroll_w;
    static constexpr int bar_x = scroll_x - 2 - 12;

    // A sent or a received message.
    struct Line {
        Time created;
        const TxMessage *tx;
        const RxMessage *rx;
    };

    RecordingScreen recording_screen_{*app_, this};

    const Peer *peer_ = nullptr;
    // Both rings, merged in time order; a member, too big for the App stack.
    std::array<Line, 64> lines_{};
    int lines_n_ = 0;
    int cursor_ = 0;    // The selected line.
    int first_ = 0;     // The line at the top.
    bool follow_ = true;// On the last line, so it moves on to a new message.

    void draw_line(int row, const Line &line);

    void play_selected();
};

class HomeScreen : public Screen {
public:
    ~HomeScreen() = default;

    HomeScreen(App &app) : Screen(app, nullptr) {}

    void init() override;

    void draw() override;

    void on_press(input::Button button) override;

    // Shows the peer's online state, signal level and pending messages as
    // they are now.
    void on_peer_updated(const Peer &peer);

private:
    MainMenuScreen main_menu_screen_{*app_, this};
    ChatScreen chat_screen_{*app_, this};

    std::array<MenuItem, PEERS_MAX_NUM> peer_items_;
    // Each peer item's value: "<snr> TR", T while sending a message, R while receiving
    std::array<std::array<char, 12>, PEERS_MAX_NUM> peer_status_{};
    Menu menu_{*app_, nullptr, {}, this};

    void record_message_start();

    void open_chat();

    // Sets the peer's row from its state, without drawing it: init() runs
    // before the menu has its items. Returns the row.
    size_t update_peer_item(const Peer &peer);
};

}// namespace pika::app
