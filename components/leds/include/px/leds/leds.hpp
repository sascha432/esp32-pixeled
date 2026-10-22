#pragma once

// The LED engine (docs/leds.md section 3): one render task owns the LED state. It draws the
// animation on the canvas, fades the brightness, maps the canvas to the LEDs through the layout,
// limits the power and sends the frame. Other tasks post commands (the functions below return
// at once) and read a copy of the state (status()).
//
// Stored in the settings NVS, namespace "leds": the layout, the config and the overlay at once, the
// state (on, brightness, animation, color, speed) and the animations' styles 10 s after their last
// change (set_blob_later), the mark "switched off by motion" when it changes (key "boot").
//
// The LED supply is switched through the callback main gives: on before the first frame, then a
// settle time; off after the LEDs faded out and a dark frame was sent.

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "px/effects/effects.hpp"
#include "px/layout/layout.hpp"
#include "px/output/output.hpp"

namespace px::leds {

constexpr size_t kMaxPanels = 16;

// The stored blobs start with their version: a struct that gets a field may keep its size
// (padding), so the size alone does not tell an old blob from a new one. Another version is not
// read: the defaults apply. Raise it with every change of Layout, State or Lanes; Config and
// Styles have their own (the task reads their previous version and keeps its values).
constexpr uint8_t kStoredVersion = 2;
constexpr uint8_t kConfigVersion = 3;

// What the LEDs do when the device starts (Config::power_on)
enum class PowerOn : uint8_t {
    // as they were: on or off, and off by motion stays off by motion (on with the next motion)
    Restore,
    Off,
    On,
};

struct Layout {
    uint8_t version = kStoredVersion;
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t panel_count = 0;
    layout::Panel panels[kMaxPanels] = {};
};

struct Config {
    uint8_t version = kConfigVersion;
    // the power limit of the frames, 0: none (docs/leds.md section 3)
    uint32_t power_limit_mw = 10000;
    // the power model: one color channel of one LED at full, and an LED while dark
    uint16_t channel_mw = 80;
    uint16_t idle_mw = 5;
    // frames per second the task aims at (the output may reach fewer); dithering wants many
    uint8_t fps = 100;
    // from dark to full brightness and back
    uint16_t fade_ms = 700;
    // after the supply is switched on, before the first frame
    uint16_t settle_ms = 50;
    // the output: the parallel output where the chip has one (RMT on the others), or RMT
    // (docs/leds.md section 2.2)
    output::Driver driver = output::Driver::Parallel;
    // temporal dithering: the 8 bits below the output's spread over the frames, for the dark
    // colors (docs/leds.md section 3)
    bool dither = true;
    // from one animation to the next, 0: at once
    uint16_t crossfade_ms = 1000;
    PowerOn power_on = PowerOn::Restore;
};

constexpr uint16_t kMaxCrossfadeMs = 10000;
constexpr uint16_t kMaxFadeMs = 10000;
constexpr uint32_t kMaxTransitionMs = 300000;

// The settings of one lane (docs/leds.md section 3, per-lane correction): LEDs of another kind
// on one lane (a panel with other chips) get their own color order, brightness, color balance and
// dithering. Stored apart from the layout (key "lanes").
struct LaneSettings {
    output::ColorOrder order = output::ColorOrder::GRB;
    // the lane's brightness against the others, 255: the same
    uint8_t level = 255;
    // the color balance, 255: the channel as it is
    uint8_t red = 255;
    uint8_t green = 255;
    uint8_t blue = 255;
    // its LEDs take temporal dithering (with Config::dither on)
    bool dither = true;
};

struct Lanes {
    uint8_t version = kStoredVersion;
    LaneSettings lane[output::kMaxLanes] = {};
};

// Motion auto off (docs/leds.md section 9): one timer. Every motion the sensor sees sets it back
// to `minutes`; when it runs out without another motion the sensor's detected state clears and
// the LEDs fade off. The next motion switches them on again, but only when motion switched them
// off: a manual off stays off. `minutes` is the sensor's hold time (px::motion, through
// Start::set_motion_hold). Stored apart (key "motion").
struct AutoOff {
    uint8_t version = 1;
    bool enabled = false;
    uint16_t minutes = 5;
};

constexpr uint16_t kMaxAutoOffMinutes = 240;

struct State {
    uint8_t version = kStoredVersion;
    bool on = false;
    uint8_t brightness = 128;
    uint8_t effect = 0;
    effects::Rgb color = {255, 120, 40};
    uint8_t speed = 128;
};

// The settings of each animation (docs/leds.md sections 6.2 and 6.4): its rotation, whether it
// follows the audio, how strongly and how fast, its palette. Indexed like px::effects; stored apart
// (key "styles").
constexpr size_t kMaxEffects = 32;

struct Style {
    // quarter turns clockwise, 0..3
    uint8_t rotation = 0;
    // an animation that can react follows the audio (effects::Reactive::Optional)
    bool reactive = false;
    // how strongly it follows: 128 as built, 255 twice as much (effects::Params::strength)
    uint8_t strength = 128;
    // an index of effects::palette_info, 0: its own colors (effects::Params::palette)
    uint8_t palette = 0;
    // how fast what it hears rises and falls, ms (effects::Params::attack_ms, release_ms)
    uint16_t attack_ms = 20;
    uint16_t release_ms = 250;
};

struct Styles {
    uint8_t version = 4;
    Style style[kMaxEffects] = {};
};

// Text or the clock over every animation on a matrix (docs/leds.md section 10). Stored apart (key
// "overlay").
enum class OverlayMode : uint8_t {
    Off,
    Text,
    Clock,
};

constexpr size_t kOverlayTextSize = 64;

struct Overlay {
    uint8_t version = 1;
    OverlayMode mode = OverlayMode::Off;
    effects::Rgb color = {255, 255, 255};
    // a text wider than the canvas scrolls: 0 stands, 255 about 50 pixels a second
    uint8_t speed = 96;
    // the clock with 24 hours, else 12
    bool clock_24h = true;
    // UTF-8
    char text[kOverlayTextSize] = {};
};

struct Status {
    State state;
    // the current animation's style
    Style style;
    // an audio source is there (the microphone), and it hears more than silence
    bool has_audio;
    bool hears_audio;
    uint16_t width;
    uint16_t height;
    uint32_t leds;
    uint8_t lanes;
    // the lanes the board has pins for
    uint8_t available_lanes;
    // the brightness the fade has reached
    uint8_t level;
    bool supply_on;
    // frames per second over the last second
    uint16_t fps;
    // the time of the last frame's steps: the animation, the frame (gamma, brightness, layout,
    // dithering, power limit) and the output until it was out
    uint32_t render_us;
    uint32_t compose_us;
    uint32_t send_us;
    // the estimate of the last frame after the limit, and whether the limit cut it
    uint32_t power_mw;
    bool limited;
    // why nothing is shown ("no layout", "lane 4 has no pin"), empty while it runs
    char error[96];
    // motion auto off: the board has a sensor, what it reports, the seconds until its hold runs
    // out and the LEDs go off (0: it sees motion, or no auto off), and whether motion switched
    // them off
    bool has_motion;
    bool motion;
    uint32_t auto_off_in_s;
    bool off_by_motion;
};

struct Start {
    // the data pin of lane n, -1 after the last
    const int8_t *lane_gpios;
    size_t lane_count;
    // switches the LED supply; nullptr without a switch
    void (*supply)(bool on);
    // the board has a motion sensor (motion auto off)
    bool has_motion = false;
    // the sensor's hold time, set from AutoOff::minutes; nullptr without a sensor
    void (*set_motion_hold)(uint32_t seconds) = nullptr;
    // the seconds the sensor's detected state still holds, 0 while it sees motion or is clear
    uint32_t (*motion_hold_left)() = nullptr;
    // the latest audio analysis, taken with every frame; nullptr without an audio input
    void (*audio)(effects::Audio &out) = nullptr;
    // an audio source is set up (the microphone, or a UDP input switched on); nullptr: `audio`
    // alone decides
    bool (*has_audio)() = nullptr;
};

esp_err_t start(const Start &config);
bool running();

void set_on(bool on);
void set_brightness(uint8_t brightness);
// an index of px::effects; ignored when there is none
void set_effect(size_t index);
void set_color(effects::Rgb color);
void set_speed(uint8_t speed);
// The next change of the brightness or of on/off takes `ms` from where the light is to where it
// goes, instead of the fade time (Home Assistant's transition); at most kMaxTransitionMs.
void set_transition(uint32_t ms);
// The buttons and the encoder (px::input): on when off and off when on; the brightness by `steps`
// (each about a sixteenth more or less, at least 1; up switches the LEDs on when they are off); the
// next (`steps` > 0) or previous animation, without the test patterns and the ones that need a sound
// source the device does not have
void toggle();
void step_brightness(int steps);
void step_effect(int steps);
// the rotation, the reactive switch and the strength of animation `effect`; ignored for one that is not there
void set_style(size_t effect, Style style);
Styles styles();

// Checks the layout (px::layout::build) and, when it is usable, hands it to the task, which
// stores it. false with the problem in `error` otherwise.
bool set_layout(const Layout &layout, char *error, size_t error_size);
Layout layout();

// false with the problem in `error` for a value out of range
bool set_config(const Config &config, char *error, size_t error_size);
Config config();

void set_lanes(const Lanes &lanes);
Lanes lanes();

void set_overlay(const Overlay &overlay);
Overlay overlay();

// The canvas as last drawn (the animation, the crossfade, the overlay; before the brightness), RGB
// row by row, into `out` of `size` bytes. The task copies it only while someone asks (within the
// last 2 s), so the first call may find nothing yet. false without a canvas or when it does not
// fit.
bool preview(uint8_t *out, size_t size, uint16_t &width, uint16_t &height);

void set_auto_off(const AutoOff &auto_off);
AutoOff auto_off();
// the motion sensor's detected state (any task); also once with the state at start
void motion(bool detected);

Status status();

// The console's output commands take the lanes for themselves: the task stops sending while
// suspended and sets its lanes up again when resumed.
void suspend(bool suspended);

} // namespace px::leds
