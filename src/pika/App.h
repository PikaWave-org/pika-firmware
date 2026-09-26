#pragma once

#include "MAC.h"
#include "radio/SX1262.h"


#include <pika/audio/ADCMicrophone.h>
#include <pika/audio/DACSpeaker.h>
#include <pika/audio/PCMPlayer.h>
#include <pika/audio/tone_generator.h>
#include <pika/buttons.h>
#include <pika/lcd/ST75160.h>
#include <pika/lcd/images/pika_logo.h>
#include <pika/log.h>

#include <ch.h>
#include <ch.hpp>
#include <hal.h>

#include <cstddef>
#include <cstdio>

namespace pika {

class App;

struct MenuItem {
    const char *name;
    void (*on_activate)(void *screen) = nullptr;
};

class Screen {
public:
    void init_activate() {
        init();
        activate();
    }

    void activate();

    virtual ~Screen() = default;

    virtual void init() { menu_cursor_ = 0; }

    virtual void draw() = 0;

    virtual void on_button(input::Button button) = 0;

protected:
    static constexpr int text_row_h = 10;
    static constexpr int menu_label_x = 16;

    App *app_{};
    Screen *parent_screen_{};
    int menu_cursor_ = -1;

    Screen(App &app, Screen *parent_screen) : app_(&app), parent_screen_(parent_screen) {}

    void draw_menu(std::span<MenuItem> items);

    void update_menu_cursor(int menu_cursor);

    void handle_menu_button(std::span<MenuItem> items, input::Button button);
};

class AboutScreen : public Screen {
public:
    AboutScreen(App &app, Screen *parent_screen) : Screen(app, parent_screen) {}

    void draw() override;

    void on_button(input::Button button) override;
};

class SettingsScreen : public Screen {
public:
    SettingsScreen(App &app, Screen *parent_screen) : Screen(app, parent_screen) {}

    void draw() override;

    void on_button(input::Button button) override;

private:
    static constexpr int backlight_steps_num = 2;
    static constexpr int backlight_step = 1000 / backlight_steps_num;

    std::array<MenuItem, 2> menu_items{
            {{"Backlight", [](void *self) { ((SettingsScreen *) self)->set_backlight(); }}, {"Dummy", [](void *) {}}}};

    void set_backlight();
};

class HomeScreen : public Screen {
public:
    ~HomeScreen() = default;

    HomeScreen(App &app) : Screen(app, nullptr) {}

    void draw() override;

    void on_button(input::Button button) override;

private:
    AboutScreen about_screen_{*app_, this};
    SettingsScreen settings_screen_{*app_, this};

    std::array<MenuItem, 3> menu_items{{
            {"Send", [](void *self) { ((HomeScreen *) self)->send_message(); }},
            {"Settings", [](void *self) { ((HomeScreen *) self)->settings_screen_.init_activate(); }},
            {"About", [](void *self) { ((HomeScreen *) self)->about_screen_.init_activate(); }},
    }};

    void send_message();
};

class App : public chibios_rt::BaseStaticThread<1024> {
public:
    struct Config {
        lcd::ST75160 *display;
        audio::DACSpeaker *speaker;
        audio::ADCMicrophone *mic;
        input::Buttons::Config buttons;
        MAC<radio::SX1262> *mac;
        PWMDriver *backlight_pwm;
        pwmchannel_t backlight_ch;
    };

    explicit App(const Config &cfg) : cfg_(cfg), buttons_(cfg.buttons) {}

    void main() override;

    void init();

    void activate_screen(Screen &screen) {
        cfg_.display->clear();
        active_screen_ = &screen;
        active_screen_->draw();
        cfg_.display->flush();
    }

    lcd::ST75160 &display() { return *cfg_.display; }

    MAC<radio::SX1262> &mac() { return *cfg_.mac; }

    int get_backlight() { return backlight_brightness_; }

    void set_backlight(int brightness);

private:
    Config cfg_;
    input::Buttons buttons_;
    audio::PCMPlayer pcm_player;
    audio::ToneGenerator tone_gen;

    HomeScreen home_screen_{*this};
    Screen *active_screen_{&home_screen_};
    int backlight_brightness_{};
};

}// namespace pika
