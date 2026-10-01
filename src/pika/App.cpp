#include "App.h"

namespace pika {

void Screen::activate() { app_->activate_screen(*this); }

void Screen::draw_menu(std::span<MenuItem> items, const char *title) {
    app_->display().rect<true>(0, 0, 160, menu_title_h);
    app_->display().text<false>(2, menu_title_y_offs, title);

    for (size_t i = 0; i < items.size(); i++) {
        const int row_y = (int) i * text_row_h + menu_items_y_offs;
        app_->display().text(menu_label_x, row_y, items[i].name);
    }
    update_menu_cursor(menu_cursor_);
}

void Screen::update_menu_cursor(int menu_cursor) {
    if (menu_cursor != menu_cursor_) {
        app_->display().text<false>(4, menu_cursor_ * text_row_h + menu_items_y_offs, ">");
    }
    menu_cursor_ = menu_cursor;
    app_->display().text(4, menu_cursor_ * text_row_h + menu_items_y_offs, ">");
}

bool Screen::handle_menu_button(std::span<MenuItem> items, input::Button button) {
    switch (button) {
        case input::Button::up:
            if (menu_cursor_ > 0) {
                update_menu_cursor(menu_cursor_ - 1);
            }
            return true;
        case input::Button::down:
            if (menu_cursor_ < items.size() - 1) {
                update_menu_cursor(menu_cursor_ + 1);
            }
            return true;
        case input::Button::left:
            if (parent_screen_) {
                parent_screen_->activate();
            }
            return true;
        case input::Button::right:
            items[menu_cursor_].on_activate(this);
            return true;
        default:
            return false;
    }
}

void AboutScreen::draw() {
    app_->display().clear();
    app_->display().bitmap(0, 0, pika::lcd::pika_logo_width, pika::lcd::pika_logo_height, pika::lcd::pika_logo);
    app_->display().text(80, 92, BOARD_NAME);
}

void AboutScreen::on_button(input::Button) {
    // Return back on any button
    parent_screen_->activate();
}

void SettingsScreen::draw() { draw_menu(menu_items, "Settings"); }

void SettingsScreen::on_button(input::Button button) { handle_menu_button(menu_items, button); }

void SettingsScreen::set_backlight() {
    int b = (app_->get_backlight() / backlight_step) + 1;
    if (b > backlight_steps_num) {
        b = 0;
    }
    app_->set_backlight(b * backlight_step);
}

void MainMenuScreen::draw() { draw_menu(menu_items, "Main Menu"); }

void MainMenuScreen::on_button(input::Button button) { handle_menu_button(menu_items, button); }

void HomeScreen::draw() { draw_menu(contacts, "Talk"); }

void HomeScreen::on_button(input::Button button) {
    switch (button) {
        case input::Button::ptt:
            send_message();
            return;
        case input::Button::left:
            main_menu_screen_.init_activate();
            return;
        default:
            break;
    }
    handle_menu_button(contacts, button);
}

void HomeScreen::send_message() {
    std::array<uint8_t, 32> msg;
    bool ok = app_->mac().send_data_frame(menu_cursor_ + 1, msg);
    LOG("send message to %s: %s", contacts[menu_cursor_].name, ok ? "OK" : "FAIL");
}

void App::main() {
    LOG("app main start");
    event_listener_t listener;
    chEvtRegister(&buttons_.events(), &listener, 0);

    while (true) {
        chEvtWaitAny(EVENT_MASK(0));

        const eventflags_t flags = chEvtGetAndClearFlags(&listener);

        for (unsigned i = 0; i < input::Buttons::count; i++) {
            const auto btn = (input::Button) i;

            if ((flags & buttons_.flag(btn)) == 0U) {
                continue;
            }

            if (active_screen_ != nullptr) {
                LOG("on_button %s", input::Buttons::name(btn));
                active_screen_->on_button(btn);
                cfg_.display->flush();
            }
        }
    }
}

void App::init() {
    LOG("app init");
    set_backlight(500);
    buttons_.init();
    active_screen_->init();
    activate_screen(home_screen_);
}

void App::set_backlight(int brightness) {
    LOG("Setting backlight to %d/%d", brightness, cfg_.backlight_pwm->config->period);
    backlight_brightness_ = brightness;
    pwmEnableChannel(cfg_.backlight_pwm, cfg_.backlight_ch, brightness);
}

}// namespace pika
