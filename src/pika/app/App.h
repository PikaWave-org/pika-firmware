#pragma once

#include "Peer.h"
#include "ui.h"
#include <pika/FixedVector.h>
#include <pika/MAC.h>
#include <pika/audio/ADCMicrophone.h>
#include <pika/audio/DACSpeaker.h>
#include <pika/audio/MELPeRecorder.h>
#include <pika/audio/PCMPlayer.h>
#include <pika/audio/tone_generator.h>
#include <pika/buttons.h>
#include <pika/gfx/Canvas.h>
#include <pika/lcd/ST75160.h>
#include <pika/power/BQ25895.h>
#include <pika/power/BatteryStatus.h>
#include <pika/proto/app.h>
#include <pika/radio/SX1262.h>
#include <pika/storage/FileWriter.h>

#include <ch.hpp>
#include <hal.h>

#include <cstddef>
#include <cstdio>

namespace pika::app {

class App : public chibios_rt::BaseStaticThread<1024>, public audio::MELPeSink, public audio::MELPeSource, public FrameHandler {
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

    // Records a message to /<peer id>/tx/<message id>; false if it could not start.
    bool start_recording(size_t peer_index);

    void finish_recording();

    std::span<Peer> peers() { return std::span(peers_); }

    Peer *get_peer(Address addr) {
        for (auto &peer: peers_) {
            if (peer.addr == addr) {
                return &peer;
            }
        }
        return nullptr;
    }

    void on_data_frame_sent(Address addr) override;
    void on_data_frame_received(Address addr, std::span<const uint8_t> frame) override;

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
    FixedVector<Peer, PEERS_MAX_NUM> peers_;

    // Reads /contacts.txt from the SD card, one "id,name,key" per line.
    void load_peers();

    // Codec thread: the recorded stream to the file, and the file back for replay.
    void on_melpe_frame(std::span<const uint8_t> frame) override;
    bool fill_melpe_frame(std::span<uint8_t> frame) override;

    Peer *recording_peer_ = nullptr;
    uint32_t recording_id_ = 0;
    char msg_path_[32]{};
    volatile uint32_t dropped_frames_ = 0;
    FileWriter<2048> msg_writer_;
    FIL replay_file_{};
    volatile bool replay_open_ = false;

    FixedVector<uint8_t, 256> tx_buf_;

    void send_messages(Peer &peer);

    void handle_message(Peer &peer, std::span<const uint8_t> data);

    void on_message(Peer &peer, const types::app::message_new &msg_new);
    void on_message(Peer &peer, const types::app::message_get &msg_get);
    void on_message(Peer &peer, const types::app::message_data &msg_data);
};

}// namespace pika::app
