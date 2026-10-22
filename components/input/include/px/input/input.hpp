#pragma once

// Buttons and a rotary encoder (docs/leds.md section 9): each button has an action for a click, a
// double click and a hold; the encoder's knob changes the brightness (or the animation, after the
// "knob" action), its push switch is a button like the others. An esp_timer samples the pins every
// 10 ms and calls the action handler from its task. The encoder counts with the PCNT where the chip
// has one, else with GPIO interrupts (the C3).
//
// The actions are settings (NVS namespace "input", key "settings", written at once: an explicit
// Save), set on the LEDs page (GET/POST /api/input) and in the backup (section "input").

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace px::input {

constexpr size_t kMaxButtons = 4;
// the encoder's push switch, after the buttons in Settings::button
constexpr size_t kEncoderButton = kMaxButtons;

// What a button does; the slugs are in the API and the backup (action_slug). A new one goes at the
// end: the index is stored.
enum class Action : uint8_t {
    None,
    OnOff,
    NextAnimation,
    PreviousAnimation,
    NextPreset,
    // as a hold they repeat while the button is held
    BrightnessUp,
    BrightnessDown,
    // as a hold after 3 s instead of 0.6 s
    Restart,
    // the hotspot, open, as when the button is held at boot (px::network::open_hotspot)
    Hotspot,
    // the encoder's knob changes the animation instead of the brightness, until it rests for 10 s
    Knob,
};
constexpr size_t kActionCount = 10;

struct Actions {
    Action click = Action::None;
    Action double_click = Action::None;
    Action hold = Action::None;
};

struct Settings {
    uint8_t version = 1;
    // the buttons, then the encoder's push switch
    Actions button[kMaxButtons + 1] = {};
};

// The defaults of a new device: button 1 the next animation (held: restart), 2 and 3 the brightness
// down and up, 4 on/off (held: the next preset); the encoder's push on/off, twice the next preset,
// held the knob for the animations (the WLED controller's 4 buttons in this order).
Settings defaults();

const char *action_slug(Action action);
// the text in the web UI
const char *action_name(Action action);
// false when `slug` is no action
bool find_action(const char *slug, Action &out);

// `steps`: for the brightness the encoder's detents (accelerated) or 1 per click and repeat; for
// the animations +1 or -1
using Handler = void (*)(Action action, int steps);

struct Pins {
    const int8_t *buttons;
    size_t button_count;
    int8_t encoder_a;
    int8_t encoder_b;
    int8_t encoder_button;
    // the button (0: button 1) that opens the hotspot when held at boot (main checks it), for the
    // status
    uint8_t hotspot_button = 0;
};

// Starts sampling the pins (the ones that are -1 are not there). Nothing to start without any:
// ESP_ERR_NOT_FOUND.
esp_err_t start(const Pins &pins, Handler handler);
bool running();

// Whether `gpio` (a button, low while pressed) is held for `ms` from now; at once false when it is
// not pressed. Called once at boot, before start(), for the hotspot.
bool held_at_boot(int8_t gpio, uint32_t ms);

Settings settings();
// false with the problem in `error` (an action that cannot be one there)
bool set_settings(const Settings &settings, char *error, size_t error_size);

struct Status {
    bool running;
    // the pin of each button and of the encoder, -1: none
    int8_t button_gpio[kMaxButtons];
    int8_t encoder_a;
    int8_t encoder_b;
    int8_t encoder_button;
    // the button (0: button 1) that opens the hotspot when held at boot
    uint8_t hotspot_button;
    // pressed now
    bool pressed[kMaxButtons + 1];
    // the encoder's position in detents since the start, and whether the knob is on the animations
    int32_t encoder_position;
    bool knob_on_animations;
};

Status status();

// GET /api/input, POST /api/input (docs/ui/web/web-app.md section 4); main registers it
void register_routes(void *server);

} // namespace px::input
