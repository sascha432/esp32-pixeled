// The LED outputs (px/output/output.hpp): the lanes, and the backend that sends them.

#include "px/output/output.hpp"

#include "esp_check.h"

#include "backends.hpp"

namespace px::output {

namespace {

constexpr const char *kTag = "output";

Lane g_lanes[kMaxLanes] = {};
size_t g_count = 0;
Driver g_driver = Driver::Rmt;

} // namespace

namespace detail {

void reorder(ColorOrder order, const uint8_t *rgb, uint8_t *out, size_t pixels)
{
    // the byte positions of R, G and B in the order the chip takes them
    static constexpr uint8_t kIndex[][3] = {
        {1, 0, 2}, // GRB
        {0, 1, 2}, // RGB
        {2, 0, 1}, // BRG
        {0, 2, 1}, // RBG
        {1, 2, 0}, // GBR
        {2, 1, 0}, // BGR
    };
    const uint8_t *index = kIndex[static_cast<size_t>(order)];
    for (size_t i = 0; i < pixels; i++, rgb += 3, out += 3) {
        out[0] = rgb[index[0]];
        out[1] = rgb[index[1]];
        out[2] = rgb[index[2]];
    }
}

} // namespace detail

bool parallel_supported()
{
    return detail::i2s::supported();
}

esp_err_t start(const Lane *lanes, size_t count, Driver driver)
{
    stop();
    ESP_RETURN_ON_FALSE(count > 0 && count <= kMaxLanes, ESP_ERR_INVALID_ARG, kTag, "%u lanes", static_cast<unsigned>(count));
    if (driver == Driver::Parallel && !parallel_supported()) {
        ESP_LOGW(kTag, "no parallel output on this chip, RMT instead");
        driver = Driver::Rmt;
    }
    const esp_err_t err = driver == Driver::Parallel ? detail::i2s::start(lanes, count) : detail::rmt::start(lanes, count);
    if (err != ESP_OK) {
        return err;
    }
    for (size_t i = 0; i < count; i++) {
        g_lanes[i] = lanes[i];
    }
    g_count = count;
    g_driver = driver;
    return ESP_OK;
}

void stop()
{
    if (g_count == 0) {
        return;
    }
    if (g_driver == Driver::Parallel) {
        detail::i2s::stop();
    } else {
        detail::rmt::stop();
    }
    g_count = 0;
}

bool running()
{
    return g_count > 0;
}

Driver driver()
{
    return g_driver;
}

size_t lane_count()
{
    return g_count;
}

const Lane &lane(size_t index)
{
    return g_lanes[index];
}

size_t pixel_count()
{
    size_t total = 0;
    for (size_t i = 0; i < g_count; i++) {
        total += g_lanes[i].pixels;
    }
    return total;
}

uint32_t encoder_max_us()
{
    return g_count > 0 && g_driver == Driver::Parallel ? detail::i2s::encoder_max_us() : 0;
}

std::expected<uint32_t, esp_err_t> encoder_test()
{
    if (g_count == 0) {
        return std::unexpected(ESP_ERR_INVALID_STATE);
    }
    if (g_driver != Driver::Parallel) {
        return std::unexpected(ESP_ERR_NOT_SUPPORTED);
    }
    return detail::i2s::encoder_test();
}

esp_err_t show(const uint8_t *rgb, size_t length, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(g_count > 0, ESP_ERR_INVALID_STATE, kTag, "not started");
    ESP_RETURN_ON_FALSE(length >= pixel_count() * 3, ESP_ERR_INVALID_SIZE, kTag, "frame too short");
    return g_driver == Driver::Parallel ? detail::i2s::show(rgb, timeout_ms) : detail::rmt::show(rgb, timeout_ms);
}

} // namespace px::output
