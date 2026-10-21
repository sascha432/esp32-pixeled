#pragma once

// The backends of px::output (docs/leds.md section 2.2). output.cpp keeps the lanes and picks one;
// a backend gets the frame of all lanes (3 bytes per LED, R G B) and puts it into the order of
// each lane's chip itself (reorder()).

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "px/output/output.hpp"

namespace px::output::detail {

// `pixels` pixels from R G B into the order the chip takes them
void reorder(ColorOrder order, const uint8_t *rgb, uint8_t *out, size_t pixels);

namespace rmt {
esp_err_t start(const Lane *lanes, size_t count);
void stop();
esp_err_t show(const uint8_t *rgb, uint32_t timeout_ms);
} // namespace rmt

namespace i2s {
// false on chips without the I2S LCD mode (only the ESP32 has it)
bool supported();
constexpr size_t kMaxLanes = 16;
esp_err_t start(const Lane *lanes, size_t count);
void stop();
esp_err_t show(const uint8_t *rgb, uint32_t timeout_ms);
} // namespace i2s

} // namespace px::output::detail
