#pragma once

// Relays on GPIOs (docs/architecture.md section 9.7a): relay n (1 to kMaxRelays) is a GPIO, high =
// on, through a transistor or a relay board: the LED power supply (main), switched by the LEDs and
// the console `relay`. Every pin is low before it becomes an output, so a relay is off while the
// board starts. A relay on the pullup (a module without its own, closed while its input is high)
// is open drain with the internal pullup: low opens it, released closes it, and it is closed (on)
// while the board starts. An active low relay (an inverting driver) is high before its pin becomes
// an output, and on while low.

#include <cstddef>

#include "esp_err.h"

namespace px::relay {

constexpr size_t kMaxRelays = 4;

// Relay n+1 on `gpios[n]` for n < `count`; a negative entry: no relay n+1. `pullups[n]`: relay
// n+1 on the internal pullup, open drain. `active_low[n]`: relay n+1 is on while its pin is low
// (push-pull; ignored with the pullup).
esp_err_t start(const signed char *gpios, const bool *pullups, const bool *active_low, size_t count);

// At least one relay is set up.
bool running();

// Relay `relay` (1-based) on or off; false when there is no such relay. From any task.
bool set(unsigned relay, bool on);

// Whether relay `relay` is on, read from its pin; false for a relay that is not there.
bool is_on(unsigned relay);

// The GPIO of relay `relay`, -1 when there is none.
int gpio_of(unsigned relay);

} // namespace px::relay
