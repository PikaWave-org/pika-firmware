#include <cmath>

#include "chprintf.h"

#include <pika/log.h>

#include "ui.h"

namespace pika::ui {
namespace {

/* Row pitch for text, and the geometry of the volume bar under it. */
constexpr int row_h = 10;
constexpr int bar_h = 14;
constexpr int bar_inset = 2;

/* Where a menu row's label and its value start, measured from the left edge.
   The cursor sits in the gap before the label. */
constexpr int label_x = 16;
constexpr int value_x = label_x + 8 * 11;

enum class ItemId : uint8_t { backlight, test_sound, back };

struct Item {
    const char *label;
    ItemId id;
};

/* The menu. A new entry is a row here and a case in activate(). */
constexpr Item menu[] = {
        {"Backlight", ItemId::backlight},
        {"Test sound", ItemId::test_sound},
        {"Back", ItemId::back},
};

constexpr size_t menu_count = sizeof menu / sizeof menu[0];

}  // namespace

Ui::Ui(const Config &cfg)
        : cfg_(cfg), buttons_(cfg.buttons), dirty_(true), volume_step_(volume_initial),
          screen_(Screen::home), cursor_(0U), backlight_(true) {}

void Ui::init() {
    buttons_.init();

    cfg_.speaker->set_volume(volume_gain(volume_step_));
    dirty_ = true;
}

/* PWR, PTT and LSN are deliberately unmapped: they are not navigation. */
Ui::Action Ui::action_of(pika::input::Button b) {
    switch (b) {
    case pika::input::Button::up:
        return Action::up;
    case pika::input::Button::down:
        return Action::down;
    case pika::input::Button::right:
        return Action::select;
    case pika::input::Button::left:
        return Action::back;
    default:
        return Action::none;
    }
}

/*
 * Fader position to gain. Loudness is logarithmic, so above the knee each step
 * is a fixed number of decibels and every press changes loudness equally.
 *
 * Below the knee it goes linear to zero: an exponential approaches silence
 * without reaching it, so otherwise the lowest setting would stay faintly
 * audible and the control could never turn the speaker off.
 */
float Ui::volume_gain(uint8_t step) {
    const float knee_db = -volume_db_per_step * (float) (volume_steps - volume_knee_step);
    const float knee_gain = powf(10.0f, knee_db / 20.0f);

    if (step <= volume_knee_step) { return knee_gain * (float) step / (float) volume_knee_step; }

    /* Anchored so that the top step lands on exactly full scale. */
    return powf(10.0f, -volume_db_per_step * (float) (volume_steps - step) / 20.0f);
}

unsigned Ui::volume_percent() const { return (unsigned) volume_step_ * 100U / volume_steps; }

void Ui::adjust_volume(int delta) {
    int step = (int) volume_step_ + delta;

    if (step < 0) { step = 0; }
    if (step > volume_steps) { step = volume_steps; }

    if (step == (int) volume_step_) { return; }

    volume_step_ = (uint8_t) step;

    const float gain = volume_gain((uint8_t) step);
    cfg_.speaker->set_volume(gain);

    /* Both go out - they are far apart on this curve, and one alone makes the
       other look wrong. No floating point in chprintf(), so gain is per mille. */
    LOG("ui: volume %u%% gain %u/1000", volume_percent(), (unsigned) (gain * 1000.0f + 0.5f));

    dirty_ = true;
}

void Ui::activate() {
    switch (menu[cursor_].id) {
    case ItemId::backlight:
        backlight_ = !backlight_;
        cfg_.lcd->backlight(backlight_);
        LOG("ui: backlight %s", backlight_ ? "on" : "off");
        break;

    case ItemId::test_sound:
        if (cfg_.test_sound != nullptr) { cfg_.test_sound(); }
        break;

    case ItemId::back:
        screen_ = Screen::home;
        break;
    }

    dirty_ = true;
}

void Ui::handle(Action a) {
    if (a == Action::none) { return; }

    if (screen_ == Screen::home) {
        switch (a) {
        case Action::up:
            adjust_volume(+1);
            break;

        case Action::down:
            adjust_volume(-1);
            break;

        case Action::select:
            screen_ = Screen::menu;
            cursor_ = 0U;
            dirty_ = true;
            break;

        default:
            break;
        }

        return;
    }

    switch (a) {
    /* Stops at the ends rather than wrapping: on a short list wrapping makes
       it easy to overshoot what you were aiming at. */
    case Action::up:
        if (cursor_ > 0U) {
            cursor_ = cursor_ - 1U;
            dirty_ = true;
        }
        break;

    case Action::down:
        if (cursor_ + 1U < menu_count) {
            cursor_ = cursor_ + 1U;
            dirty_ = true;
        }
        break;

    case Action::select:
        activate();
        break;

    case Action::back:
        screen_ = Screen::home;
        dirty_ = true;
        break;

    default:
        break;
    }
}

void Ui::run() {
    event_listener_t listener;
    chEvtRegister(&buttons_.events(), &listener, 0);

    while (true) {
        chEvtWaitAny(EVENT_MASK(0));

        const eventflags_t flags = chEvtGetAndClearFlags(&listener);

        for (unsigned i = 0; i < pika::input::Buttons::count; i++) {
            const auto btn = (pika::input::Button) i;

            if ((flags & pika::input::Buttons::flag(btn)) == 0U) { continue; }

            LOG("btn: %s pressed", pika::input::Buttons::name(btn));
            handle(action_of(btn));
        }
    }
}

/* The bar shows fader position, not gain: it tracks the knob, as a volume
   control is expected to. The fill is inset so it never touches the outline,
   which would make an almost empty bar hard to tell from an empty one. */
void Ui::draw_home() {
    char line[16];
    chsnprintf(line, sizeof line, "vol %u%%", volume_percent());
    cfg_.lcd->text(4, cfg_.y, line);

    const int bar_y = cfg_.y + row_h + 4;
    const int bar_w = pika::lcd::ST75160::width - 8;

    cfg_.lcd->frame(4, bar_y, bar_w, bar_h, true);

    const int inner = bar_w - 2 * bar_inset;
    const int fill = inner * (int) volume_step_ / volume_steps;

    if (fill > 0) {
        cfg_.lcd->rect(4 + bar_inset, bar_y + bar_inset, fill, bar_h - 2 * bar_inset, true);
    }
}

/* One row per item, a cursor on the selected one, and a value on the right
   for the items that carry state. */
void Ui::draw_menu() {
    for (size_t i = 0; i < menu_count; i++) {
        const int row_y = cfg_.y + (int) i * row_h;

        if (i == cursor_) { cfg_.lcd->text(4, row_y, ">"); }

        cfg_.lcd->text(label_x, row_y, menu[i].label);

        if (menu[i].id == ItemId::backlight) {
            cfg_.lcd->text(value_x, row_y, backlight_ ? "on" : "off");
        }
    }
}

void Ui::draw() {
    /* The UI owns everything from cfg_.y down, and clearing all of it is
       simpler than tracking which fields shrank since the last draw. */
    cfg_.lcd->rect(0, cfg_.y, pika::lcd::ST75160::width, pika::lcd::ST75160::height - cfg_.y, false);

    if (screen_ == Screen::home) {
        draw_home();
    } else {
        draw_menu();
    }
}

}  // namespace pika::ui
