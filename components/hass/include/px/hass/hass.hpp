#pragma once

// MQTT and Home Assistant discovery (docs/leds.md section 8). One connection to a broker
// (espressif/mqtt runs it in its own task and reconnects by itself). On every connect the device
// announces itself: availability "online" (the last will says "offline"), the discovery configs
// of its entities (retained, under the discovery prefix), the subscriptions of the command
// topics, the state. Commands go to the render task (px::leds); a timer publishes what changed.
//
// Entities: a light (JSON schema: on, brightness, RGB color, the animations as effects), a
// number (speed), sensors (power in W, frames per second), a binary sensor (motion, with a
// sensor), the current animation's rotation and sound settings, the overlay, the power on
// behavior and a button (restart). The table is in docs/leds.md section 8.
//
// Stored in the settings NVS, namespace "mqtt": the settings as a blob, the password apart (it
// never leaves the device).

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace px::hass {

constexpr size_t kUriSize = 128;
constexpr size_t kUserSize = 65;
constexpr size_t kPasswordSize = 65;
constexpr size_t kTopicSize = 65;

struct Settings {
    uint8_t version = 1;
    bool enabled = false;
    // mqtt://host:1883 or mqtts://host:8883
    char uri[kUriSize] = {};
    char username[kUserSize] = {};
    // Home Assistant's discovery prefix
    char prefix[kTopicSize] = "homeassistant";
    // the topics of this device; empty: pixeled/<device name>
    char base[kTopicSize] = {};
};

enum class State : uint8_t {
    Off,
    Connecting,
    Connected,
    Disconnected,
};

struct Status {
    State state;
    // the last problem, for people ("the broker refused the login"), empty while connected
    char error[96];
    uint32_t connected_s;
    // the topics in use
    char base[kTopicSize];
};

const char *state_name(State state);

struct Device {
    const char *name;     // pixeled-xxxx: the client id and the node id
    const char *board;    // the model in Home Assistant
    const char *firmware; // the version
    bool has_motion;      // the binary sensor is announced
    // an audio source is set up: "Reacts to sound", its strength, attack and release are announced,
    // "Spectrum" is in the effect list; asked again with every check (a UDP input switched on
    // announces them); nullptr: none
    bool (*has_audio)();
};

// Loads the settings and connects when enabled. Call after the network and the LEDs started.
esp_err_t start(const Device &device);

// Checks and stores the settings and connects again with them. `password` nullptr keeps the
// stored one, "" removes it. false with the problem in `error`.
bool set_settings(const Settings &settings, const char *password, char *error, size_t error_size);
Settings settings();
bool password_set();

Status status();

// the motion sensor's state for its binary sensor (sent when connected, again after a reconnect)
void publish_motion(bool detected);

// the routes of /api/mqtt on the web server (an httpd_handle_t), main firmware only
void register_routes(void *server);

} // namespace px::hass
