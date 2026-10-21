#pragma once

// The audio input (docs/leds.md section 6.1): an I2S microphone (INMP441 or alike) on the I2S1 of
// the ESP32 (I2S0 drives the LEDs). A task reads blocks of 512 samples at 22 kHz (23 ms each) and
// analyzes them: loudness, 32 bands, the beat (analysis.hpp). The render task takes the latest
// result with every frame (frame()).
//
// The microphone's L/R pin decides the slot it sends in, and the ESP32 swaps the two in some
// modes: both slots are read and the one with the signal is used.

#include <cstdint>

#include "esp_err.h"
#include "px/effects/effects.hpp"

namespace px::audio {

struct Config {
    int sck;
    int ws;
    int sd;
};

esp_err_t start(const Config &config);
bool running();

// The latest analysis; `active` false without a running input or in silence.
void frame(effects::Audio &out);

struct Status {
    bool running;
    // the loudest band of the last block and the automatic gain's reference, dB of full scale
    float loudest_db;
    float reference_db;
    // the slot the microphone sends in: 'L' or 'R', '?' before it was found
    char slot;
    uint32_t blocks;
    effects::Audio audio;
};

Status status();

} // namespace px::audio
