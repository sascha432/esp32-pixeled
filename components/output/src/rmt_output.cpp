// The RMT backend of px::output (docs/leds.md section 2.2): one TX channel per lane. A lane's frame
// is reordered into a buffer of its own (the channel reads it while sending) and encoded by a
// bytes encoder with the WS2812 bit timings, followed by the latch.

#include "backends.hpp"

#include <cstring>

#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "soc/soc_caps.h"

namespace px::output::detail::rmt {

namespace {

constexpr const char *kTag = "output";

// 10 MHz: 0.1 µs per tick
constexpr uint32_t kResolutionHz = 10 * 1000 * 1000;
// WS2812B: 0 = 0.3 µs high, 0.9 µs low; 1 = 0.9 µs high, 0.3 µs low (1.2 µs a bit)
constexpr rmt_symbol_word_t kZero = {.duration0 = 3, .level0 = 1, .duration1 = 9, .level1 = 0};
constexpr rmt_symbol_word_t kOne = {.duration0 = 9, .level0 = 1, .duration1 = 3, .level1 = 0};
// the latch: newer WS2812B need at least 280 µs low
constexpr uint16_t kLatchTicks = 3000;

// TX channels of the chip (IDF 6.1 keeps the number in its private HAL, hal/rmt_ll.h)
#if CONFIG_IDF_TARGET_ESP32
constexpr size_t kMaxChannels = 8;
#elif CONFIG_IDF_TARGET_ESP32S3
constexpr size_t kMaxChannels = 4;
#else
constexpr size_t kMaxChannels = 2;
#endif

// The encoder: the pixel bytes, then one symbol that holds the line low for the latch
struct LedEncoder {
    rmt_encoder_t base;
    rmt_encoder_handle_t bytes;
    rmt_encoder_handle_t copy;
    int state;
    rmt_symbol_word_t latch;
};

// runs in the RMT interrupt, also while the flash is busy (CONFIG_RMT_TX_ISR_CACHE_SAFE): IRAM
IRAM_ATTR size_t encode(rmt_encoder_t *encoder, rmt_channel_handle_t channel, const void *data, size_t size,
                        rmt_encode_state_t *out_state)
{
    auto *self = __containerof(encoder, LedEncoder, base);
    rmt_encode_state_t session = RMT_ENCODING_RESET;
    int state = RMT_ENCODING_RESET;
    size_t symbols = 0;
    if (self->state == 0) {
        symbols += self->bytes->encode(self->bytes, channel, data, size, &session);
        if (session & RMT_ENCODING_COMPLETE) {
            self->state = 1;
        }
        if (session & RMT_ENCODING_MEM_FULL) {
            *out_state = static_cast<rmt_encode_state_t>(state | RMT_ENCODING_MEM_FULL);
            return symbols;
        }
    }
    if (self->state == 1) {
        symbols += self->copy->encode(self->copy, channel, &self->latch, sizeof(self->latch), &session);
        if (session & RMT_ENCODING_COMPLETE) {
            self->state = RMT_ENCODING_RESET;
            state |= RMT_ENCODING_COMPLETE;
        }
        if (session & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
        }
    }
    *out_state = static_cast<rmt_encode_state_t>(state);
    return symbols;
}

IRAM_ATTR esp_err_t reset_encoder(rmt_encoder_t *encoder)
{
    auto *self = __containerof(encoder, LedEncoder, base);
    rmt_encoder_reset(self->bytes);
    rmt_encoder_reset(self->copy);
    self->state = RMT_ENCODING_RESET;
    return ESP_OK;
}

esp_err_t delete_encoder(rmt_encoder_t *encoder)
{
    auto *self = __containerof(encoder, LedEncoder, base);
    rmt_del_encoder(self->bytes);
    rmt_del_encoder(self->copy);
    heap_caps_free(self);
    return ESP_OK;
}

esp_err_t new_encoder(rmt_encoder_handle_t &out)
{
    // the interrupt reads it: internal RAM
    auto *self = static_cast<LedEncoder *>(heap_caps_calloc(1, sizeof(LedEncoder), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (self == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    self->base.encode = encode;
    self->base.reset = reset_encoder;
    self->base.del = delete_encoder;
    self->latch = {.duration0 = kLatchTicks / 2, .level0 = 0, .duration1 = kLatchTicks / 2, .level1 = 0};
    rmt_bytes_encoder_config_t bytes{};
    bytes.bit0 = kZero;
    bytes.bit1 = kOne;
    bytes.flags.msb_first = 1;
    rmt_copy_encoder_config_t copy{};
    if (rmt_new_bytes_encoder(&bytes, &self->bytes) != ESP_OK || rmt_new_copy_encoder(&copy, &self->copy) != ESP_OK) {
        delete_encoder(&self->base);
        return ESP_ERR_NO_MEM;
    }
    out = &self->base;
    return ESP_OK;
}

struct Channel {
    Lane lane;
    rmt_channel_handle_t handle;
    rmt_encoder_handle_t encoder;
    uint8_t *buffer;
};

Channel g_channels[kMaxChannels] = {};
size_t g_count = 0;

} // namespace

esp_err_t start(const Lane *lanes, size_t count)
{
    stop();
    ESP_RETURN_ON_FALSE(count <= kMaxChannels, ESP_ERR_NOT_SUPPORTED, kTag, "%u lanes, the RMT has %u channels",
                        static_cast<unsigned>(count), static_cast<unsigned>(kMaxChannels));
    for (size_t i = 0; i < count; i++) {
        Channel &c = g_channels[i];
        c.lane = lanes[i];
        rmt_tx_channel_config_t config{};
        config.gpio_num = static_cast<gpio_num_t>(lanes[i].gpio);
        config.clk_src = RMT_CLK_SRC_DEFAULT;
        config.resolution_hz = kResolutionHz;
        // The pulse memory of the unused channels goes to the lanes: the interrupt refills half of
        // it at a time, so more memory is more time before a late refill shows on the LEDs
        // (64 symbols: about 40 us; with 3 lanes on the ESP32 each gets 128).
        const size_t blocks = kMaxChannels / count;
        config.mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL * (blocks > 0 ? blocks : 1);
        // the highest level an interrupt handler written in C may have
        config.intr_priority = 3;
        config.trans_queue_depth = 2;
        // the buffer is read while sending: internal RAM, also on boards with PSRAM
        c.buffer = static_cast<uint8_t *>(heap_caps_calloc(1, lanes[i].pixels * 3u + 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        esp_err_t err = c.buffer != nullptr ? ESP_OK : ESP_ERR_NO_MEM;
        if (err == ESP_OK) {
            err = rmt_new_tx_channel(&config, &c.handle);
            if (err != ESP_OK && config.mem_block_symbols > SOC_RMT_MEM_WORDS_PER_CHANNEL) {
                // the memory is not free in one piece: one block, as many lanes as channels
                config.mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL;
                err = rmt_new_tx_channel(&config, &c.handle);
            }
        }
        if (err == ESP_OK) {
            err = new_encoder(c.encoder);
        }
        if (err == ESP_OK) {
            err = rmt_enable(c.handle);
        }
        g_count = i + 1;
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "lane %u on GPIO %d: %s", static_cast<unsigned>(i + 1), lanes[i].gpio, esp_err_to_name(err));
            stop();
            return err;
        }
        ESP_LOGI(kTag, "lane %u on GPIO %d, %u pixels (RMT)", static_cast<unsigned>(i + 1), lanes[i].gpio,
                 lanes[i].pixels);
    }
    return ESP_OK;
}

void stop()
{
    for (size_t i = 0; i < g_count; i++) {
        Channel &c = g_channels[i];
        if (c.handle != nullptr) {
            rmt_disable(c.handle);
            rmt_del_channel(c.handle);
        }
        if (c.encoder != nullptr) {
            rmt_del_encoder(c.encoder);
        }
        heap_caps_free(c.buffer);
        c = {};
    }
    g_count = 0;
}

esp_err_t show(const uint8_t *rgb, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(g_count > 0, ESP_ERR_INVALID_STATE, kTag, "not started");
    const rmt_transmit_config_t transmit{};
    for (size_t i = 0; i < g_count; i++) {
        Channel &c = g_channels[i];
        reorder(c.lane.order, rgb, c.buffer, c.lane.pixels);
        rgb += c.lane.pixels * 3u;
        ESP_RETURN_ON_ERROR(rmt_transmit(c.handle, c.encoder, c.buffer, c.lane.pixels * 3u, &transmit), kTag,
                            "lane %u", static_cast<unsigned>(i + 1));
    }
    for (size_t i = 0; i < g_count; i++) {
        ESP_RETURN_ON_ERROR(rmt_tx_wait_all_done(g_channels[i].handle, static_cast<int>(timeout_ms)), kTag,
                            "lane %u timed out", static_cast<unsigned>(i + 1));
    }
    return ESP_OK;
}

} // namespace px::output::detail::rmt
