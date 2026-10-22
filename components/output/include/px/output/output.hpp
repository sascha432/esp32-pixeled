#pragma once

// The LED outputs (docs/leds.md section 2): WS2812-type data on up to kMaxLanes pins (lanes),
// each lane with its own pixel count and color order. One owner calls it (the render task; until
// it exists the console's `leds` command); nothing here is thread safe.
//
// Backends: RMT, one channel per lane (every chip; the ESP32 has 8 TX channels, the S3 4, the C3
// and C6 2), and the parallel output: the ESP32's I2S0 in LCD mode or the S3's LCD_CAM, all lanes
// from one DMA stream, no interrupt per pixel. PARLIO (C6) is not built yet.

#include <cstddef>
#include <cstdint>
#include <expected>

#include "esp_err.h"

namespace px::output {

constexpr size_t kMaxLanes = 16;

// the order the LED chip takes the colors in; WS2812B: GRB
enum class ColorOrder : uint8_t {
    GRB,
    RGB,
    BRG,
    RBG,
    GBR,
    BGR,
};

enum class Driver : uint8_t {
    Rmt,
    // one DMA stream for all lanes; RMT on a chip without it
    Parallel,
};

struct Lane {
    int gpio;
    uint16_t pixels;
    ColorOrder order = ColorOrder::GRB;
};

// Sets up the lanes; a running output is stopped first. ESP_ERR_NOT_SUPPORTED with more lanes
// than the backend has channels.
esp_err_t start(const Lane *lanes, size_t count, Driver driver = Driver::Rmt);

// the chip has a parallel output
bool parallel_supported();
// the backend of the running output
Driver driver();

// Releases the channels; the pins go back to inputs.
void stop();

bool running();
size_t lane_count();
const Lane &lane(size_t index);

// Sends one frame and waits until every lane is out (at most `timeout_ms`). `rgb` holds the
// pixels of lane 1, then lane 2, ..., 3 bytes each in R, G, B; the backend puts them into the
// lane's color order.
esp_err_t show(const uint8_t *rgb, size_t length, uint32_t timeout_ms = 100);

// the pixels of all lanes together
size_t pixel_count();

// The parallel backend encodes while it sends: the longest time its interrupt took in the last
// frame, in us (0 on RMT). Above 2.2 ms (three buffers) the DMA would catch up.
uint32_t encoder_max_us();
// Times the parallel encoder alone, without sending: every buffer of a frame with every bit of
// every lane set (the slowest data), no LED gets it. The longest buffer in us;
// ESP_ERR_NOT_SUPPORTED on RMT.
std::expected<uint32_t, esp_err_t> encoder_test();

} // namespace px::output
