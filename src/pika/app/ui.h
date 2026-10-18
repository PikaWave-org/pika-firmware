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
    // 8x8, drawn at the right end of the row; none if null.
    const uint8_t *icon = nullptr;
    // Short text right-aligned before the icon; none if null.
    const char *value = nullptr;
};

// A titled list of items with a cursor; activating an item calls it with owner.
class Menu {
public:
    Menu(App &app, const char *title, std::span<const MenuItem> items, void *owner) :
            app_(&app), title_(title), items_(items), owner_(owner) {}

    void set_items(std::span<const MenuItem> items) { items_ = items; }

    void reset() { cursor_ = 0; }

    int cursor() const { return cursor_; }

    bool empty() const { return items_.empty(); }

    void draw();

    // Redraws one row, after its item changed.
    void draw_item(size_t i);

    // Up, down and right; returns false for any other button.
    bool on_press(input::Button button);

private:
    static constexpr int row_h = 10;
    static constexpr int label_x = 16;
    static constexpr int title_h = 11;
    static constexpr int title_y = StatusBar::status_bar_h;
    static constexpr int items_y = title_y + title_h + 1;
    static constexpr int icon_size = 8;
    static constexpr int icon_x = 160 - 2 - icon_size;

    App *app_;
    const char *title_;
    std::span<const MenuItem> items_;
    void *owner_;
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

class SettingsScreen : public Screen {
public:
    SettingsScreen(App &app, Screen *parent_screen) : Screen(app, parent_screen) {}

    void init() override { menu_.reset(); }

    void draw() override;

    void on_press(input::Button button) override;

private:
    static constexpr int backlight_steps_num = 2;
    static constexpr int backlight_step = 1000 / backlight_steps_num;

    std::array<MenuItem, 2> menu_items{
            {{"Backlight", [](void *self) { ((SettingsScreen *) self)->set_backlight(); }}, {"Dummy", [](void *) {}}}};
    Menu menu_{*app_, "Settings", menu_items, this};

    void set_backlight();
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

class HomeScreen : public Screen {
public:
    ~HomeScreen() = default;

    HomeScreen(App &app) : Screen(app, nullptr) {}

    void init() override;

    void draw() override;

    void on_press(input::Button button) override;

    // Shows the peer's online state and message counts as they are now.
    void on_peer_updated(Address peer_addr);

private:
    MainMenuScreen main_menu_screen_{*app_, this};
    RecordingScreen recording_screen_{*app_, this};

    std::array<MenuItem, PEERS_MAX_NUM> peer_items_;
    // "R<rx> T<tx>", the value of each peer's item.
    std::array<std::array<char, 12>, PEERS_MAX_NUM> peer_counts_{};
    Menu menu_{*app_, "Peers", {}, this};

    void record_message_start();

    void update_item(size_t i);
};

}// namespace pika::app
