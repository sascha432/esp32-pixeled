// The parallel backend of px::output on the ESP32 (docs/leds.md section 2.2): I2S0 in LCD mode
// sends 16-bit samples, bit n of a sample on lane n, from one DMA chain. A WS2812 bit is three
// samples (slots) at 2.4 MHz, 0.417 us each: high, the data bit, low. So a 0 is 0.42 us high and
// 0.83 us low, a 1 is 0.83 us high and 0.42 us low, all lanes at once.
//
// The frame is encoded into a buffer in internal DMA memory: per LED position 24 bits x 3 slots x
// 2 bytes = 144 bytes for all lanes together, the slots that are always high or low written once
// when the lanes are set up. Zeros at the end hold the lines low for the latch. Then the DMA sends
// it without the CPU; the interrupt at its end only wakes the caller. The I2S1 stays for the
// microphone.

#include "backends.hpp"

#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32

#include <cstring>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_private/periph_ctrl.h"
#include "esp_rom_gpio.h"
#include "esp_rom_lldesc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hal/i2s_ll.h"
#include "soc/gpio_sig_map.h"
#include "soc/i2s_reg.h"
#include "soc/i2s_struct.h"

namespace px::output::detail::i2s {

namespace {

constexpr const char *kTag = "output";

// The I2S hardware takes the 16-bit samples of a 32-bit word in the other order: the second one
// is sent first. The encoder writes each slot to its partner's place (found on the test matrix).
constexpr bool kSwapHalves = true;

constexpr size_t kSlotsPerBit = 3;
constexpr size_t kSlotsPerLed = 24 * kSlotsPerBit;
// the latch: 800 slots of 0.417 us, 333 us (the WS2812B needs 280)
constexpr size_t kLatchSlots = 800;
// one DMA descriptor carries at most 4095 bytes; a multiple of 4
constexpr size_t kChunk = 4092;

i2s_dev_t *const g_hw = &I2S0;
Lane g_lanes[kMaxLanes] = {};
size_t g_count = 0;
size_t g_positions = 0; // the LEDs of the longest lane
uint16_t *g_slots = nullptr;
size_t g_slot_count = 0;
lldesc_t *g_descriptors = nullptr;
size_t g_descriptor_count = 0;
uint8_t *g_reordered = nullptr; // one lane's LEDs in its chip's order
intr_handle_t g_interrupt = nullptr;
SemaphoreHandle_t g_done = nullptr;

size_t slot_index(size_t i)
{
    return kSwapHalves ? i ^ 1 : i;
}

IRAM_ATTR void on_interrupt(void *)
{
    const uint32_t status = g_hw->int_st.val;
    g_hw->int_clr.val = status;
    if ((status & I2S_OUT_TOTAL_EOF_INT_ST) != 0) {
        g_hw->conf.tx_start = 0;
        BaseType_t woken = pdFALSE;
        xSemaphoreGiveFromISR(g_done, &woken);
        if (woken == pdTRUE) {
            portYIELD_FROM_ISR();
        }
    }
}

void configure_hardware()
{
    PERIPH_RCC_ATOMIC()
    {
        i2s_ll_enable_bus_clock(0, true);
        i2s_ll_reset_register(0);
        i2s_ll_enable_core_clock(g_hw, true);
    }
    i2s_dev_t *hw = g_hw;
    hw->conf.val = 0;
    hw->conf.tx_reset = 1;
    hw->conf.tx_reset = 0;
    hw->conf.tx_fifo_reset = 1;
    hw->conf.tx_fifo_reset = 0;
    hw->lc_conf.val = 0;
    hw->lc_conf.out_rst = 1;
    hw->lc_conf.out_rst = 0;
    hw->lc_conf.ahbm_rst = 1;
    hw->lc_conf.ahbm_rst = 0;
    hw->lc_conf.ahbm_fifo_rst = 1;
    hw->lc_conf.ahbm_fifo_rst = 0;

    hw->conf.tx_msb_right = 1;
    hw->conf.tx_right_first = 1;
    // parallel output: LCD mode, one sample per clock
    hw->conf2.val = 0;
    hw->conf2.lcd_en = 1;
    hw->conf2.lcd_tx_wrx2_en = 0;
    hw->conf2.lcd_tx_sdx2_en = 0;
    hw->sample_rate_conf.val = 0;
    i2s_ll_tx_set_bits_mod(hw, 16);
    i2s_ll_tx_set_bck_div_num(hw, 1);
    // 2.4 MHz from the 160 MHz PLL: 160 / 2 / (33 + 1/3); the LCD mode halves the clock (measured
    // 2026-10-19: 256 LEDs per lane in 8.33 ms, 7.68 ms of them data)
    i2s_ll_tx_clk_set_src(hw, I2S_CLK_SRC_DEFAULT);
    i2s_ll_set_raw_mclk_div(hw, 33, 3, 1);
    hw->fifo_conf.val = 0;
    hw->fifo_conf.tx_fifo_mod_force_en = 1;
    hw->fifo_conf.tx_fifo_mod = 1; // 16 bit, one channel
    hw->fifo_conf.tx_data_num = 32;
    hw->fifo_conf.dscr_en = 1;
    hw->conf1.val = 0;
    hw->conf1.tx_stop_en = 0;
    hw->conf1.tx_pcm_bypass = 1;
    hw->conf_chan.val = 0;
    hw->conf_chan.tx_chan_mod = 1;
    hw->timing.val = 0;
    hw->lc_conf.outdscr_burst_en = 1;
    hw->lc_conf.out_data_burst_en = 1;
    hw->int_ena.val = 0;
    hw->int_clr.val = 0xffffffff;
}

void free_buffers()
{
    heap_caps_free(g_slots);
    heap_caps_free(g_descriptors);
    heap_caps_free(g_reordered);
    g_slots = nullptr;
    g_descriptors = nullptr;
    g_reordered = nullptr;
}

} // namespace

bool supported()
{
    return true;
}

esp_err_t start(const Lane *lanes, size_t count)
{
    stop();
    ESP_RETURN_ON_FALSE(count <= kMaxLanes, ESP_ERR_NOT_SUPPORTED, kTag, "%u lanes, I2S has 16",
                        static_cast<unsigned>(count));
    g_positions = 0;
    for (size_t i = 0; i < count; i++) {
        g_lanes[i] = lanes[i];
        g_positions = lanes[i].pixels > g_positions ? lanes[i].pixels : g_positions;
    }
    g_count = count;
    g_slot_count = g_positions * kSlotsPerLed + kLatchSlots;
    g_slot_count = (g_slot_count + 1) & ~static_cast<size_t>(1); // whole 32-bit words
    const size_t bytes = g_slot_count * sizeof(uint16_t);
    g_descriptor_count = (bytes + kChunk - 1) / kChunk;
    g_slots = static_cast<uint16_t *>(heap_caps_calloc(1, bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    g_descriptors = static_cast<lldesc_t *>(
        heap_caps_calloc(g_descriptor_count, sizeof(lldesc_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    g_reordered = static_cast<uint8_t *>(heap_caps_malloc(g_positions * 3, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (g_done == nullptr) {
        g_done = xSemaphoreCreateBinary();
    }
    if (g_slots == nullptr || g_descriptors == nullptr || g_reordered == nullptr || g_done == nullptr) {
        ESP_LOGE(kTag, "no internal DMA memory for %u bytes (%u LEDs per lane)", static_cast<unsigned>(bytes),
                 static_cast<unsigned>(g_positions));
        free_buffers();
        g_count = 0;
        return ESP_ERR_NO_MEM;
    }

    // the slots that never change: the first of every bit high on every lane, the third low
    uint16_t lanes_mask = 0;
    for (size_t i = 0; i < count; i++) {
        lanes_mask |= static_cast<uint16_t>(1u << i);
    }
    for (size_t bit = 0; bit < g_positions * 24; bit++) {
        g_slots[slot_index(bit * kSlotsPerBit)] = lanes_mask;
    }
    // the chain of descriptors over the buffer, the last one ends the frame
    const auto *data = reinterpret_cast<const uint8_t *>(g_slots);
    for (size_t i = 0; i < g_descriptor_count; i++) {
        lldesc_t &d = g_descriptors[i];
        const size_t length = i + 1 < g_descriptor_count ? kChunk : bytes - i * kChunk;
        d.size = static_cast<uint32_t>(length);
        d.length = static_cast<uint32_t>(length);
        d.owner = 1;
        d.sosf = 0;
        d.eof = i + 1 == g_descriptor_count ? 1 : 0;
        d.buf = data + i * kChunk;
        d.empty = i + 1 < g_descriptor_count ? reinterpret_cast<uint32_t>(&g_descriptors[i + 1]) : 0;
    }

    configure_hardware();
    for (size_t i = 0; i < count; i++) {
        const auto pin = static_cast<gpio_num_t>(lanes[i].gpio);
        gpio_set_level(pin, 0);
        gpio_set_direction(pin, GPIO_MODE_OUTPUT);
        esp_rom_gpio_pad_select_gpio(pin);
        // the 16-bit samples come out on the data lines 8 to 23
        esp_rom_gpio_connect_out_signal(pin, I2S0O_DATA_OUT8_IDX + i, false, false);
        ESP_LOGI(kTag, "lane %u on GPIO %d, %u pixels (I2S)", static_cast<unsigned>(i + 1), lanes[i].gpio,
                 lanes[i].pixels);
    }
    ESP_RETURN_ON_ERROR(esp_intr_alloc(ETS_I2S0_INTR_SOURCE, ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL3, on_interrupt,
                                       nullptr, &g_interrupt),
                        kTag, "interrupt");
    ESP_LOGI(kTag, "I2S buffer %u bytes in %u descriptors", static_cast<unsigned>(bytes),
             static_cast<unsigned>(g_descriptor_count));
    return ESP_OK;
}

void stop()
{
    if (g_count == 0) {
        return;
    }
    g_hw->conf.tx_start = 0;
    g_hw->out_link.stop = 1;
    g_hw->int_ena.val = 0;
    if (g_interrupt != nullptr) {
        esp_intr_free(g_interrupt);
        g_interrupt = nullptr;
    }
    for (size_t i = 0; i < g_count; i++) {
        const auto pin = static_cast<gpio_num_t>(g_lanes[i].gpio);
        esp_rom_gpio_connect_out_signal(pin, SIG_GPIO_OUT_IDX, false, false);
        gpio_set_level(pin, 0);
    }
    free_buffers();
    g_count = 0;
}

esp_err_t show(const uint8_t *rgb, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(g_count > 0, ESP_ERR_INVALID_STATE, kTag, "not started");
    // the data slots: bit b of LED p of lane n is bit n of slot (p * 24 + b) * 3 + 1
    for (size_t bit = 0; bit < g_positions * 24; bit++) {
        g_slots[slot_index(bit * kSlotsPerBit + 1)] = 0;
    }
    for (size_t lane = 0; lane < g_count; lane++) {
        const size_t pixels = g_lanes[lane].pixels;
        reorder(g_lanes[lane].order, rgb, g_reordered, pixels);
        rgb += pixels * 3;
        const auto mask = static_cast<uint16_t>(1u << lane);
        for (size_t byte = 0; byte < pixels * 3; byte++) {
            const uint8_t value = g_reordered[byte];
            if (value == 0) {
                continue;
            }
            const size_t first = byte * 8;
            for (size_t b = 0; b < 8; b++) {
                if ((value & (0x80 >> b)) != 0) {
                    g_slots[slot_index((first + b) * kSlotsPerBit + 1)] |= mask;
                }
            }
        }
    }

    xSemaphoreTake(g_done, 0);
    i2s_dev_t *hw = g_hw;
    hw->conf.tx_start = 0;
    hw->conf.tx_reset = 1;
    hw->conf.tx_reset = 0;
    hw->conf.tx_fifo_reset = 1;
    hw->conf.tx_fifo_reset = 0;
    hw->lc_conf.out_rst = 1;
    hw->lc_conf.out_rst = 0;
    hw->int_clr.val = 0xffffffff;
    hw->int_ena.out_total_eof = 1;
    hw->out_link.addr = reinterpret_cast<uint32_t>(&g_descriptors[0]) & 0xfffff;
    hw->out_link.start = 1;
    hw->conf.tx_start = 1;
    if (xSemaphoreTake(g_done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        hw->conf.tx_start = 0;
        ESP_LOGW(kTag, "the I2S frame did not end");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

} // namespace px::output::detail::i2s

#else

namespace px::output::detail::i2s {

bool supported()
{
    return false;
}

esp_err_t start(const Lane *, size_t)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void stop()
{
}

esp_err_t show(const uint8_t *, uint32_t)
{
    return ESP_ERR_NOT_SUPPORTED;
}

} // namespace px::output::detail::i2s

#endif
