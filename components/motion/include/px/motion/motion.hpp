#pragma once

// A motion sensor (PIR) on a GPIO (docs/architecture.md section 9.7b): the board names the pin and
// the level its output has while it sees motion (px::board::Info::motion_gpio, motion_active_high).
// The pin is sampled every 100 ms; a level that holds for two samples is the sensor's state. The
// detected state follows it with a hold: it turns on with the sensor and turns off only after the
// sensor saw nothing for the hold time (a sensor that resets after 1-2 s would flicker otherwise).
// main hands every change of the detected state to the LEDs (auto off) and to MQTT.

#include <cstdint>

#include "esp_err.h"

namespace px::motion {

// Called from the esp_timer task with every change of the detected state, and once with the state
// at start.
using Handler = void (*)(bool detected);

constexpr uint32_t kMinHoldS = 10;
constexpr uint32_t kMaxHoldS = 240 * 60;
constexpr uint32_t kDefaultHoldS = 300;

// The sensor on `gpio`, `active_high`: its output is high while it sees motion. The pin gets the
// internal pull toward the other level, so an unplugged sensor reads clear.
esp_err_t start(int gpio, bool active_high, Handler on_change);

// A sensor is set up.
bool running();

// The sensor's state now, without the hold (false without a sensor).
bool occupied();

// The state with the hold, the one the handler gets (false without a sensor).
bool detected();

// How long the detected state stays on after the sensor's last motion; clamped to kMinHoldS to
// kMaxHoldS. Any time, also before start; a running hold counts with the new time.
void set_hold_s(uint32_t seconds);
uint32_t hold_s();
// The seconds until the detected state turns off without another motion, 0 while it is off or
// the sensor still sees motion (the hold has not started).
uint32_t hold_left_s();

// The GPIO of the sensor, -1 without one.
int gpio();

// Whether the sensor's output is high while it sees motion.
bool active_high();

} // namespace px::motion
