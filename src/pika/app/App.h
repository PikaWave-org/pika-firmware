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

class App : public chibios_rt::BaseStaticThread<1024>,
            public audio::MELPeSink,
            public audio::MELPeSource,
            public FrameHandler {
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

    explicit App(const Config &cfg) : cfg_(cfg), canvas_(cfg.display->framebuffer()), buttons_(cfg.buttons) {
        chEvtObjectInit(&frame_event_src_);
    }

    void main() override;

    void init();

    void activate_screen(Screen &screen) {
        canvas_.clear();
        active_screen_ = &screen;
        active_screen_->draw();
        cfg_.display->flush();
    }

    Canvas &canvas() { return canvas_; }

    bool is_active(const Screen &screen) const { return active_screen_ == &screen; }

    MAC<radio::SX1262> &mac() { return *cfg_.mac; }

    int get_backlight() { return backlight_brightness_; }

    void set_backlight(int brightness);

    void do_set_backlight(int brightness);

    // Keyboard lock, see lock_press().
    bool locked() const { return locked_; }

    const BatteryStatus &battery_status() const { return cfg_.charger->status(); }


    // Records a message to /<peer id>/tx/<message id>; false if it could not start.
    bool start_recording(size_t peer_index);

    void finish_recording();

    // Writes the current backlight and transmit power to /settings.txt.
    void save_settings();

    // Plays the newest fully received message from the peer; false if there
    // is none or it could not start.
    bool play_last_message(size_t peer_index);

    // Plays /<peer id>/tx/<message id> if sent, else .../rx/..., unless the
    // codec is busy; false if it could not start.
    bool play_message(const Peer &peer, bool sent, uint32_t msg_id);

    // Whether a message to or from the peer, or that very one, is playing.
    bool is_playing(const Peer &peer) const { return playing_peer_ == &peer; }

    bool is_playing(const Peer &peer, bool sent, uint32_t msg_id) const {
        return playing_peer_ == &peer and playing_sent_ == sent and playing_id_ == msg_id;
    }

    std::span<Peer> peers() { return std::span(peers_); }

    const Identity &self() const { return self_; }

    Peer *get_peer(Address addr) {
        for (auto &peer: peers_) {
            if (peer.addr == addr) {
                return &peer;
            }
        }
        return nullptr;
    }

    // MAC thread: queued for handle_frame_events().
    void on_data_frame_sent(Address addr, const PeerInfo &peer_info) override;
    void on_data_frame_received(Address addr, std::span<const uint8_t> data, const PeerInfo &peer_info) override;
    void on_peer_updated(Address addr, const PeerInfo &peer_info) override;

private:
    // Plays a tone and waits for it; skipped while the speaker or the codec is
    // busy. Only through the named sounds below.
    void beep(float freq_hz, float duration_s);

    // The sounds, by meaning. Recording and playback share started and stopped.
    void beep_started();
    void beep_stopped();
    void beep_delivered();
    void beep_new_message();
    void beep_locked();
    void beep_unlocked();

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

    // Restarts the backlight timeout from now; App::main applies it on the same pass.
    void wake_backlight(systime_t now) { backlight_wake_ = now; }

    // Keyboard lock: LEFT held for lock_hold on the home screen locks; LEFT
    // then RIGHT within unlock_window unlocks. While locked no press reaches a
    // screen; releases still do, so a button held through locking, like PTT
    // while recording, still ends what it started.
    static constexpr sysinterval_t lock_hold = TIME_S2I(1);
    static constexpr sysinterval_t unlock_window = TIME_S2I(1);
    static constexpr size_t MAX_MSG_DATA_SIZE = 220;

    bool locked_ = false;
    input::Button last_press_ = input::Button::count;
    systime_t last_press_at_ = 0;

    // Sees every press first; true if locked, and the screen must not see it.
    bool lock_press(input::Button btn, systime_t now);
    void check_lock_hold(systime_t now);
    sysinterval_t backlight_timeout_ = TIME_S2I(30);
    FixedVector<Peer, PEERS_MAX_NUM> peers_;

    // Reads /contacts.txt from the SD card, one "id,name,key" per line.
    void load_peers();

    // Applies /settings.txt, "key=value" lines: backlight in percent,
    // tx_power in dBm at the antenna. Missing keys keep their defaults.
    void load_settings();

    // Reads our own entry from /id.txt, "id,name,key" like a peer, and sets
    // the address in the MAC.
    void load_identity();

    Identity self_{};

    // Codec thread: the recorded stream to the file, and a received message from its file for replay.
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

    // A FrameHandler call, copied, as the received frame is only valid during the call.
    struct MACEvent {
        enum Kind : uint8_t { sent, received, peer_updated };

        static const char *name(Kind kind) {
            switch (kind) {
                case sent:
                    return "sent";
                case received:
                    return "received";
                case peer_updated:
                    return "peer updated";
            }
            return "?";
        }

        Kind kind{};
        Address addr{};
        FixedVector<uint8_t, 256> data;
        PeerInfo peer_info{};
    };

    // Written by the MAC thread, read by the App thread.
    ConcurrentRingBuffer<MACEvent, 8> frame_events_;
    event_source_t frame_event_src_;

    void post_frame_event(MACEvent::Kind kind, Address addr, const PeerInfo &peer_info, std::span<const uint8_t> data);

    void handle_frame_events();

    void send_messages(Peer &peer);

    // Reads size bytes at offset of /<peer id>/tx/<message id> into buf; false
    // if the file is missing or shorter.
    bool read_message(Address peer_addr, uint32_t msg_id, uint32_t offset, size_t size, FixedVector<uint8_t, 256> &buf);

    // Writes data at offset of /<peer id>/rx/<message id>; offset 0 creates
    // the file, and the directories on the way.
    bool write_message(Address peer_addr, uint32_t msg_id, uint32_t offset, std::span<const uint8_t> data);

    // Asks the peer for the message from size_received on; at the end, that
    // acks the whole message.
    void request_message(Peer &peer, RxMessage &msg);


    // The message play_message() started, until it has played out; no peer
    // when none.
    const Peer *playing_peer_ = nullptr;
    bool playing_sent_ = false;
    uint32_t playing_id_ = 0;

    // Beeps once a replay has played out.
    void on_recorder_idle();

    // App thread: one message chunk read or written at a time.
    FIL msg_file_{};

    void handle_message(Peer &peer, std::span<const uint8_t> data);

    void on_message(Peer &peer, const types::app::message_new &msg_new);
    void on_message(Peer &peer, const types::app::message_get &msg_get);
    void on_message(Peer &peer, const types::app::message_data &msg_data);
};

}// namespace pika::app
