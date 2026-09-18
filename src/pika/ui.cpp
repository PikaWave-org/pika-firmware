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

enum class ItemId : uint8_t { backlight, contrast, test_sound, record, back };

struct Item {
    const char *label;
    ItemId id;
};

/* The menu. A new entry is a row here and a case in activate(). */
constexpr Item menu[] = {
        {"Backlight", ItemId::backlight},
        {"Contrast", ItemId::contrast},
        {"Test sound", ItemId::test_sound},
        {"Record", ItemId::record},
        {"Back", ItemId::back},
};

constexpr size_t menu_count = sizeof menu / sizeof menu[0];

}  // namespace

Ui::Ui(const Config &cfg)
        : cfg_(cfg), buttons_(cfg.buttons), dirty_(true), volume_step_(volume_initial),
          screen_(Screen::home), cursor_(0U), backlight_step_(backlight_initial), contrast_step_(contrast_initial),
          contrast_pending_(false), editing_(false), record_gate_(false) {}

void Ui::init() {
    buttons_.init();

    cfg_.speaker->set_volume(volume_gain(volume_step_));
    set_backlight();
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

unsigned Ui::backlight_percent() const { return (unsigned) backlight_step_ * backlight_percent_per_step; }

/* Straight onto the timer channel: one register write, nothing for the panel's
   thread to arbitrate. The macro wants hundredths of a percent. */
void Ui::set_backlight() {
    pwmEnableChannel(cfg_.backlight_pwm, cfg_.backlight_ch,
                     PWM_PERCENTAGE_TO_WIDTH(cfg_.backlight_pwm, backlight_percent() * 100U));
}

void Ui::adjust_backlight(int delta) {
    int step = (int) backlight_step_ + delta;

    if (step < 0) {
        step = 0;
    }
    if (step > backlight_steps) {
        step = backlight_steps;
    }

    if (step == (int) backlight_step_) {
        return;
    }

    backlight_step_ = (uint8_t) step;
    set_backlight();

    LOG("ui: backlight %u%%", backlight_percent());

    dirty_ = true;
}

uint16_t Ui::contrast_vop(uint8_t step) { return contrast_vop_min + (uint16_t) step * contrast_vop_step; }

/* V0 = 3.6 + vop * 0.04 volts, in tenths because chprintf() has no %f. Every
   Vop here is a multiple of 10, so the division is exact. */
unsigned Ui::contrast_tenths() const { return 36U + (unsigned) contrast_vop(contrast_step_) * 4U / 10U; }

/*
 * Only records the change. Sending it is an I2C transaction that shares the
 * sequencing buffer with flush(), so it belongs to the thread that owns the
 * panel and reaches it through take_contrast().
 */
void Ui::adjust_contrast(int delta) {
    int step = (int) contrast_step_ + delta;

    if (step < 0) { step = 0; }
    if (step > contrast_steps) { step = contrast_steps; }

    if (step == (int) contrast_step_) { return; }

    contrast_step_ = (uint8_t) step;
    contrast_pending_ = true;

    const unsigned tenths = contrast_tenths();

    LOG("ui: contrast %u.%uV", tenths / 10U, tenths % 10U);

    dirty_ = true;
}

bool Ui::take_contrast(uint16_t &vop) {
    if (!contrast_pending_) { return false; }
    contrast_pending_ = false;
    vop = contrast_vop(contrast_step_);
    return true;
}

void Ui::activate() {
    switch (menu[cursor_].id) {
    case ItemId::backlight:
    case ItemId::contrast:
        editing_ = true;
        break;

    case ItemId::test_sound:
        if (cfg_.test_sound != nullptr) { cfg_.test_sound(); }
        break;

    case ItemId::record:
        screen_ = Screen::record;
        record_gate_ = false;
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

    /* The record screen is held rather than pressed, so the only press it acts
       on is the one that leaves. The press that begins a hold arrives here too
       and must do nothing: a screen that moved out from under the finger would
       stop the recording it just started. */
    if (screen_ == Screen::record) {
        if (a == Action::back) {
            screen_ = Screen::menu;
            dirty_ = true;
        }

        return;
    }

    /* Editing takes the cursor keys over, so it has to come first. Both
       select and back leave: the value is applied as it changes, so there is
       no pending edit for one of them to discard. */
    if (editing_) {
        /* Which value the cursor keys move is the row they are on. */
        const int delta = (a == Action::up) ? +1 : -1;

        switch (a) {
        case Action::up:
        case Action::down:
            if (menu[cursor_].id == ItemId::backlight) {
                adjust_backlight(delta);
            } else {
                adjust_contrast(delta);
            }
            break;

        case Action::select:
        case Action::back:
            editing_ = false;
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

/*
 * The recording control, as an intent: the caller asks whether to record, not
 * which pad is down.
 *
 * It is the select pad, and only on the record screen - the same pad means
 * "activate" everywhere else, so a hold there must not start a recording.
 * Leaving the screen therefore ends one in progress, which is the only cancel
 * this needs.
 */
bool Ui::record_held() {
    if (screen_ != Screen::record) { return false; }

    const bool down = buttons_.pressed(pika::input::Button::right);

    /* The press that opened this screen is the same pad, and on a real finger
       it is still down when the screen appears - so it has to come up once
       before a hold counts. Without the gate, arriving here records for
       however long the finger stayed on the button and then replays it, which
       is a recording nobody asked for. Measured on the board: 160ms. */
    if (!record_gate_) {
        record_gate_ = !down;
        return false;
    }

    return down;
}

/*
 * Where the current screen ends, so whoever draws below can tell whether there
 * is room left. The menu is the one that grows: it is measured from the table
 * rather than from a number written down twice, so a new row pushes this down
 * and the caller notices instead of being quietly overdrawn.
 */
int Ui::bottom_y() const {
    switch (screen_) {
    case Screen::home:
        return cfg_.y + row_h + 4 + bar_h;

    case Screen::record:
        return cfg_.y + 2 * row_h;

    default:
        return cfg_.y + (int) menu_count * row_h;
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

    cfg_.lcd->frame<true>(4, bar_y, bar_w, bar_h);

    const int inner = bar_w - 2 * bar_inset;
    const int fill = inner * (int) volume_step_ / volume_steps;

    if (fill > 0) {
        cfg_.lcd->rect<true>(4 + bar_inset, bar_y + bar_inset, fill, bar_h - 2 * bar_inset);
    }
}

/* One row per item, a cursor on the selected one, and a value on the right
   for the items that carry state. */
void Ui::draw_menu() {
    for (size_t i = 0; i < menu_count; i++) {
        const int row_y = cfg_.y + (int) i * row_h;

        if (i == cursor_) { cfg_.lcd->text(4, row_y, ">"); }

        cfg_.lcd->text(label_x, row_y, menu[i].label);

        /* Brackets mark the value the cursor keys are moving, on the row under
           the cursor only. */
        const bool edited = editing_ && (i == cursor_);

        switch (menu[i].id) {
        case ItemId::backlight: {
            char value[12];
            chsnprintf(value, sizeof value, edited ? "[%u%%]" : "%u%%", backlight_percent());
            cfg_.lcd->text(value_x, row_y, value);
            break;
        }

        case ItemId::contrast: {
            const unsigned tenths = contrast_tenths();

            char value[12];
            chsnprintf(value, sizeof value, edited ? "[%u.%uV]" : "%u.%uV", tenths / 10U, tenths % 10U);
            cfg_.lcd->text(value_x, row_y, value);
            break;
        }

        /* The rest carry no state to show. */
        default:
            break;
        }
    }
}

/* Two rows and nothing else: the level while recording is the meter block the
   caller draws below this, which stays live for as long as the button is held
   because the capture never stops to record. */
void Ui::draw_record() {
    cfg_.lcd->text(4, cfg_.y, "hold RIGHT to rec");

    if (cfg_.record_status != nullptr) { cfg_.lcd->text(4, cfg_.y + row_h, cfg_.record_status()); }
}

void Ui::draw() {
    /* The UI owns everything from cfg_.y down, and clearing all of it is
       simpler than tracking which fields shrank since the last draw. */
    cfg_.lcd->rect<false>(0, cfg_.y, pika::lcd::ST75160::width, pika::lcd::ST75160::height - cfg_.y);

    switch (screen_) {
    case Screen::home:
        draw_home();
        break;

    case Screen::record:
        draw_record();
        break;

    default:
        draw_menu();
        break;
    }
}

}  // namespace pika::ui
