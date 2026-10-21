#pragma once

// The UDP inputs' task (udp.cpp, docs/leds.md section 6.1): WARLS and WLED audio sync packets in,
// the latest audio frame out while its source holds.

#include <cstdint>

#include "esp_err.h"
#include "px/audio/audio.hpp"
#include "px/effects/effects.hpp"

namespace px::audio::detail {

// loads the inputs (start())
esp_err_t load_udp();
// starts the task once the network stack is up (start_udp()); it waits while both inputs are off
esp_err_t start_udp_task();

void set_udp_inputs(const Inputs &inputs);
Inputs udp_inputs();

// The frame of the UDP source that sent last, while it holds (the packet's timeout); false when
// none does. With `take_beat` a beat is handed out once.
bool udp_frame(effects::Audio &out, Source &source, bool take_beat);

uint32_t udp_packets();

} // namespace px::audio::detail
