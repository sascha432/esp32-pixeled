// The audio input (px/audio/audio.hpp, docs/leds.md section 6.1).

#include "px/audio/audio.hpp"

#include <cstring>

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "analysis.hpp"

namespace px::audio {

namespace {

constexpr const char *kTag = "audio";

i2s_chan_handle_t g_channel = nullptr;
SemaphoreHandle_t g_lock = nullptr;
// the task's own: the analysis and a block of both slots (32-bit samples)
detail::Analyzer g_analyzer;
int32_t g_raw[detail::kFftSize * 2];
float g_samples[detail::kFftSize];
// shared under g_lock
Status g_status = {};
// a beat since the last frame
bool g_beat_pending = false;

// the slot with the signal: the mean of the absolute values, the other slot reads 0 or noise
int choose_slot(int current)
{
    uint64_t sum[2] = {};
    for (size_t i = 0; i < detail::kFftSize; i++) {
        const int32_t left = g_raw[i * 2] >> 8;
        const int32_t right = g_raw[i * 2 + 1] >> 8;
        sum[0] += static_cast<uint64_t>(left < 0 ? -left : left);
        sum[1] += static_cast<uint64_t>(right < 0 ? -right : right);
    }
    // a slot that reads exactly 0 sends nothing; otherwise keep the one chosen until the other
    // is clearly stronger
    if (current < 0 || sum[1 - current] > sum[current] * 4) {
        return sum[1] > sum[0] ? 1 : 0;
    }
    return current;
}

void task(void *)
{
    int slot = -1;
    effects::Audio audio;
    for (;;) {
        size_t read = 0;
        if (i2s_channel_read(g_channel, g_raw, sizeof(g_raw), &read, portMAX_DELAY) != ESP_OK ||
            read != sizeof(g_raw)) {
            continue;
        }
        const int chosen = choose_slot(slot);
        if (chosen != slot) {
            ESP_LOGI(kTag, "the microphone sends in the %s slot", chosen == 0 ? "left" : "right");
            slot = chosen;
        }
        // 24 bits left aligned in 32: to -1..1
        for (size_t i = 0; i < detail::kFftSize; i++) {
            g_samples[i] = static_cast<float>(g_raw[i * 2 + slot] >> 8) / 8388608.0f;
        }
        g_analyzer.process(g_samples, audio);

        xSemaphoreTake(g_lock, portMAX_DELAY);
        g_status.audio = audio;
        // a beat stays until a frame took it (the render task may be slower than the blocks)
        g_beat_pending = g_beat_pending || audio.beat;
        g_status.loudest_db = g_analyzer.loudest_db();
        g_status.reference_db = g_analyzer.reference_db();
        g_status.slot = slot == 0 ? 'L' : 'R';
        g_status.blocks++;
        xSemaphoreGive(g_lock);
    }
}

} // namespace

esp_err_t start(const Config &config)
{
    ESP_RETURN_ON_FALSE(g_lock == nullptr, ESP_ERR_INVALID_STATE, kTag, "already started");
    g_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(g_lock != nullptr, ESP_ERR_NO_MEM, kTag, "lock");
    g_status.slot = '?';

    // I2S1: the LEDs' parallel output has I2S0 (components/output)
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    channel.dma_desc_num = 4;
    channel.dma_frame_num = 256;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel, nullptr, &g_channel), kTag, "channel");
    i2s_std_config_t std{};
    std.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(detail::kSampleRate);
    std.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
    std.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    std.gpio_cfg.bclk = static_cast<gpio_num_t>(config.sck);
    std.gpio_cfg.ws = static_cast<gpio_num_t>(config.ws);
    std.gpio_cfg.dout = I2S_GPIO_UNUSED;
    std.gpio_cfg.din = static_cast<gpio_num_t>(config.sd);
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(g_channel, &std), kTag, "std mode");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(g_channel), kTag, "enable");

    // core 0 with Wi-Fi: the render task has core 1; the FFT takes about 1 ms per block
    const BaseType_t core = portNUM_PROCESSORS > 1 ? 0 : tskNO_AFFINITY;
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(task, "audio", 4096, nullptr, 4, nullptr, core) == pdPASS,
                        ESP_ERR_NO_MEM, kTag, "task");
    g_status.running = true;
    ESP_LOGI(kTag, "microphone on SCK %d, WS %d, SD %d (I2S1, %u Hz)", config.sck, config.ws, config.sd,
             static_cast<unsigned>(detail::kSampleRate));
    return ESP_OK;
}

bool running()
{
    return g_lock != nullptr;
}

void frame(effects::Audio &out)
{
    if (g_lock == nullptr) {
        out = {};
        return;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    out = g_status.audio;
    out.beat = g_beat_pending;
    g_beat_pending = false;
    xSemaphoreGive(g_lock);
}

Status status()
{
    if (g_lock == nullptr) {
        return {};
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    const Status out = g_status;
    xSemaphoreGive(g_lock);
    return out;
}

} // namespace px::audio
