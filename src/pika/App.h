#pragma once

#include <pika/MAC.h>
#include <pika/audio/ADCMicrophone.h>
#include <pika/audio/DACSpeaker.h>
#include <pika/audio/MELPeRecorder.h>
#include <pika/audio/PCMPlayer.h>
#include <pika/audio/tone_generator.h>
#include <pika/buttons.h>
#include <pika/gfx/Canvas.h>
#include <pika/lcd/ST75160.h>
#include <pika/lcd/images/pika_logo.h>
#include <pika/log.h>
#include <pika/power/BQ25895.h>
#include <pika/power/BatteryStatus.h>
#include <pika/radio/SX1262.h>
#include <pika/storage/FileWriter.h>

#include <ch.h>
#include <ch.hpp>
#include <hal.h>

#include <cstddef>
#include <cstdio>

namespace pika {

class App;

static constexpr size_t CONTACTS_MAX_NUM = 128;

struct Contact {
    std::array<char, 128> name;
    std::array<uint8_t, 32> key;
};

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

    // Up, down and right; returns false for any other button.
    bool on_press(input::Button button);

private:
    static constexpr int row_h = 10;
    static constexpr int label_x = 16;
    static constexpr int title_h = 11;
    static constexpr int title_y = StatusBar::status_bar_h;
    static constexpr int items_y = title_y + title_h + 1;

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

    void show(const Contact &contact);

    void draw() override;

    void on_press(input::Button button) override { (void) button; }

    void on_release(input::Button button) override;

private:
    const Contact *contact_ = nullptr;
};

class HomeScreen : public Screen {
public:
    ~HomeScreen() = default;

    HomeScreen(App &app) : Screen(app, nullptr) {}

    void init() override;

    void draw() override;

    void on_press(input::Button button) override;

private:
    MainMenuScreen main_menu_screen_{*app_, this};
    RecordingScreen recording_screen_{*app_, this};

    std::array<MenuItem, CONTACTS_MAX_NUM> contacts_items_;
    Menu menu_{*app_, "Contacts", {}, this};

    void record_message_start();
};

class App : public chibios_rt::BaseStaticThread<1024> {
public:
    using Canvas = gfx::Canvas<lcd::ST75160::Framebuffer>;

    struct Config {
        lcd::ST75160 *display;
        audio::DACSpeaker *speaker;
        audio::ADCMicrophone *mic;
        audio::MELPeRecorder *recorder;
        input::Buttons::Config buttons;
        MAC<radio::SX1262> *mac;
        power::BQ25895 *charger;
        PWMDriver *backlight_pwm;
        pwmchannel_t backlight_ch;
    };

    explicit App(const Config &cfg) : cfg_(cfg), canvas_(cfg.display->framebuffer()), buttons_(cfg.buttons) {}

    void main() override;

    void init();

    void activate_screen(Screen &screen) {
        canvas_.clear();
        active_screen_ = &screen;
        active_screen_->draw();
        cfg_.display->flush();
    }

    Canvas &canvas() { return canvas_; }

    MAC<radio::SX1262> &mac() { return *cfg_.mac; }

    int get_backlight() { return backlight_brightness_; }

    void set_backlight(int brightness);

    void do_set_backlight(int brightness);

    const BatteryStatus &battery_status() const { return cfg_.charger->status(); }

    // Plays a tone without waiting for it; cuts short whatever is playing.
    void beep(float freq_hz, float duration_s);

    void start_recording(const Contact &contact);

    void finish_recording();

    std::span<const Contact> contacts() const { return {contacts_.data(), contacts_num_}; }

private:
    Config cfg_;
    Canvas canvas_;
    input::Buttons buttons_;
    audio::PCMPlayer pcm_player;
    audio::ToneGenerator tone_gen;

    StatusBar status_bar_{*this};
    HomeScreen home_screen_{*this};
    Screen *active_screen_{&home_screen_};
    int backlight_brightness_{};
    systime_t backlight_wake_ = 0;
    sysinterval_t backlight_timeout_ = TIME_S2I(5);
    std::array<Contact, CONTACTS_MAX_NUM> contacts_;
    size_t contacts_num_ = 0;

    // Reads /contacts.txt from the SD card, one "name,key" per line.
    void load_contacts();

    const Contact *recording_for_ = nullptr;
};

}// namespace pika
