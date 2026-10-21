#pragma once

// The audio input (docs/leds.md section 6.1): an I2S microphone (INMP441 or alike) on the I2S1 of
// the ESP32 (I2S0 drives the LEDs), and the UDP inputs WARLS and WLED audio sync. The
// microphone's task reads blocks of 512 samples at 22 kHz (23 ms each) and analyzes them:
// loudness, 32 bands, the beat (analysis.hpp). A second task receives the UDP packets
// (packets.hpp). The render task takes the latest result with every frame (frame()): a UDP source
// while its packets come, else the microphone.
//
// The microphone's L/R pin decides the slot it sends in, and the ESP32 swaps the two in some
// modes: both slots are read and the one with the signal is used.

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "px/effects/effects.hpp"

namespace px::audio {

// the microphone's pins; sd -1: no microphone
struct Config {
    int sck;
    int ws;
    int sd;
};

// The settings of the analysis (docs/leds.md section 6.1). Stored in the settings NVS, namespace
// "audio", key "settings"; the web UI's Microphone card, the console and the backup set them.
struct Settings {
    uint8_t version = 1;
    // added to the input, dB: moves every band, the loudness and the silence point
    int8_t gain_db = 0;
    // the reference follows the loudest band; off: the reference is full scale (0 dB), the gain
    // decides how full the bars get
    bool agc = true;
    // below it (dB of full scale, the gain added) the input is silence
    int8_t silence_db = -62;
    // the dB below the reference shown as 0..255
    uint8_t range_db = 40;
    // a beat when the bass is this many tenths of its average (16: 1.6 times)
    uint8_t beat_ratio_tenths = 16;
};

constexpr int kMinGainDb = -20;
constexpr int kMaxGainDb = 40;
constexpr int kMinSilenceDb = -90;
constexpr int kMaxSilenceDb = -20;
constexpr int kMinRangeDb = 10;
constexpr int kMaxRangeDb = 80;
constexpr int kMinBeatTenths = 12;
constexpr int kMaxBeatTenths = 30;

// The UDP inputs (docs/leds.md section 6.1): another device or a PC sends its analysis. Stored in
// the settings NVS, namespace "audio", key "inputs".
struct Inputs {
    uint8_t version = 1;
    // WARLS spectrum packets (the kfc_fw format, its scripts/audio_analyzer or tools/audio_send.py)
    bool warls = false;
    uint16_t warls_port = 21324;
    // WLED audio sync v2: multicast 239.0.0.1, port 11988 (a WLED sound reactive device, LedFx)
    bool wled_sync = false;
};

// where the audio frame comes from now
enum class Source : uint8_t {
    None,
    Microphone,
    Warls,
    WledSync,
};

const char *source_name(Source source);

// Loads the settings, starts the microphone (with its pins).
esp_err_t start(const Config &config);
// Starts the UDP inputs' task: after the network stack is up (a socket before it asserts in lwIP:
// a boot loop with an input switched on, 2026-10-21).
esp_err_t start_udp();
// start() ran
bool running();
// the microphone runs
bool has_microphone();
// an audio source is set up: the microphone, or a UDP input switched on
bool has_source();

// stored; the task opens the sockets anew within a second
void set_inputs(const Inputs &inputs);
Inputs inputs();

// false with the problem in `error` for a value out of range; stored and used from the next block
bool set_settings(const Settings &settings, char *error, size_t error_size);
Settings settings();

// GET and POST /api/audio (main registers them with the other routes)
void register_routes(void *server);

// The latest analysis; `active` false without a running input or in silence.
void frame(effects::Audio &out);

struct Status {
    // the microphone runs
    bool running;
    Source source;
    // UDP packets taken since the start
    uint32_t packets;
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
