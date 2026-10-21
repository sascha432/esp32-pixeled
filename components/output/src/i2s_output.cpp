// The parallel backend of px::output on the ESP32 (docs/leds.md section 2.2): I2S0 in LCD mode
// sends 16-bit samples, bit n of a sample on lane n. A WS2812 bit is three samples (slots) at
// 2.4 MHz, 0.417 us each: high, the data bit, low. So a 0 is 0.42 us high and 0.83 us low, a 1 is
// 0.83 us high and 0.42 us low, all lanes at once.
//
// The frame is encoded while it is sent: a ring of kRing small buffers in internal DMA memory,
// kPositions LED positions each (144 bytes per position for all lanes), the slots that are always
// high or low written once when the lanes are set up. The DMA reads the buffers in a circle; the
// interrupt at the end of each one encodes the next positions into it, so the encoder stays
// kRing - 1 buffers (2.2 ms) ahead of the DMA. After the last positions the chain goes on to the
// latch (zeros) and ends there. The memory does not grow with the LEDs: 1024 per lane take the
// same 15 KB as 32 (one buffer for the whole frame took 147 KB).
//
// 8-bit samples would not halve it: in LCD mode at 8 bits the ESP32's I2S still takes one 32-bit
// FIFO word per two samples (ESP-IDF's i80 LCD driver for the ESP32 says so), so 16 bits stay.
// The interrupt runs from IRAM, also while the flash is busy: it reads the frame from internal RAM
// (a frame in the PSRAM is copied first). The I2S1 stays for the microphone.

#include "backends.hpp"

#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32

#include <cstring>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_private/esp_clk.h"
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
// the ring: 4 buffers of 24 positions, 3456 bytes and 0.72 ms each (a descriptor carries at most
// 4095 bytes)
constexpr size_t kPositions = 24;
constexpr size_t kRing = 4;
constexpr size_t kBufferSlots = kPositions * kSlotsPerLed;

i2s_dev_t *const g_hw = &I2S0;
Lane g_lanes[kMaxLanes] = {};
size_t g_count = 0;
size_t g_positions = 0; // the LEDs of the longest lane
uint16_t *g_ring = nullptr; // kRing buffers of kBufferSlots
uint16_t *g_latch = nullptr;
lldesc_t *g_descriptors = nullptr; // the ring's, then the latch's
intr_handle_t g_interrupt = nullptr;
SemaphoreHandle_t g_done = nullptr;
// a frame from the PSRAM, copied for the interrupt
uint8_t *g_staging = nullptr;
size_t g_staging_size = 0;

// the frame being sent, the interrupt's while it runs
const uint8_t *g_lane_rgb[kMaxLanes] = {};
uint16_t g_lane_pixels[kMaxLanes] = {};
// the places of the chip's three colors in R G B, per lane
uint8_t g_order[kMaxLanes][3] = {};
volatile size_t g_next_position = 0;
// the ring buffer the interrupt fills next (the oldest one sent)
volatile size_t g_refill = 0;
// the times the DMA nearly caught up with the encoder (an interrupt late by more than 1.4 ms)
volatile uint32_t g_late = 0;
uint32_t g_late_reported = 0;
// the longest interrupt of the frame in CPU cycles, from the refill loop
volatile uint32_t g_max_cycles = 0;

constexpr size_t slot_index(size_t i)
{
    return kSwapHalves ? i ^ 1 : i;
}

// The data slots of `count` positions from `first` on into `slots`: bit b of LED p of lane n is
// bit n of slot b * 3 + 1 of the position.
IRAM_ATTR void encode(uint16_t *slots, size_t first, size_t count)
{
    for (size_t p = 0; p < count; p++, slots += kSlotsPerLed) {
        uint16_t words[24] = {};
        const size_t position = first + p;
        for (size_t lane = 0; lane < g_count; lane++) {
            if (position >= g_lane_pixels[lane]) {
                continue;
            }
            const uint8_t *pixel = g_lane_rgb[lane] + position * 3;
            const uint32_t bits = static_cast<uint32_t>(pixel[g_order[lane][0]]) << 16 |
                                  static_cast<uint32_t>(pixel[g_order[lane][1]]) << 8 | pixel[g_order[lane][2]];
            if (bits == 0) {
                continue;
            }
            const auto mask = static_cast<uint16_t>(1u << lane);
            for (size_t b = 0; b < 24; b++) {
                if ((bits & (0x800000u >> b)) != 0) {
                    words[b] |= mask;
                }
            }
        }
        for (size_t b = 0; b < 24; b++) {
            slots[slot_index(b * kSlotsPerBit + 1)] = words[b];
        }
    }
}

// The next positions into ring buffer `k`, its descriptor on to the next buffer, or to the latch
// after the last positions. false when every position is encoded.
IRAM_ATTR bool fill(size_t k)
{
    const size_t first = g_next_position;
    if (first >= g_positions) {
        return false;
    }
    const size_t count = g_positions - first < kPositions ? g_positions - first : kPositions;
    encode(g_ring + k * kBufferSlots, first, count);
    g_next_position = first + count;
    lldesc_t &d = g_descriptors[k];
    d.length = static_cast<uint32_t>(count * kSlotsPerLed * sizeof(uint16_t));
    d.owner = 1;
    d.empty = reinterpret_cast<uint32_t>(g_next_position >= g_positions ? &g_descriptors[kRing]
                                                                        : &g_descriptors[(k + 1) % kRing]);
    return true;
}

IRAM_ATTR void on_interrupt(void *)
{
    const uint32_t status = g_hw->int_st.val;
    g_hw->int_clr.val = status;
    if ((status & I2S_OUT_EOF_INT_ST) != 0) {
        // the descriptor the DMA read last: every ring buffer up to it is free again (two ends may
        // come with one interrupt)
        // compared on the 20 bits the link registers hold (the internal RAM is unique in them)
        const uint32_t done = g_hw->out_eof_des_addr & 0xfffff;
        const uint32_t first = reinterpret_cast<uint32_t>(&g_descriptors[0]) & 0xfffff;
        if (done >= first && done < first + kRing * sizeof(lldesc_t)) {
            const size_t last = (done - first) / sizeof(lldesc_t);
            const uint32_t start = esp_cpu_get_cycle_count();
            size_t filled = 0;
            while (filled < kRing) {
                const size_t k = g_refill;
                if (!fill(k)) {
                    break;
                }
                g_refill = (k + 1) % kRing;
                filled++;
                if (k == last) {
                    break;
                }
            }
            if (filled >= kRing - 1) {
                g_late = g_late + 1;
            }
            const uint32_t cycles = esp_cpu_get_cycle_count() - start;
            if (cycles > g_max_cycles) {
                g_max_cycles = cycles;
            }
        }
    }
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
    // out_eof_mode 0: a buffer's end is reported once the DMA read it, not once it was sent: the
    // encoder gets the most time
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
    heap_caps_free(g_ring);
    heap_caps_free(g_latch);
    heap_caps_free(g_descriptors);
    heap_caps_free(g_staging);
    g_ring = nullptr;
    g_latch = nullptr;
    g_descriptors = nullptr;
    g_staging = nullptr;
    g_staging_size = 0;
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
    // the places of R, G and B in the order the chip takes them (as reorder())
    static constexpr uint8_t kIndex[][3] = {{1, 0, 2}, {0, 1, 2}, {2, 0, 1}, {0, 2, 1}, {1, 2, 0}, {2, 1, 0}};
    g_positions = 0;
    for (size_t i = 0; i < count; i++) {
        g_lanes[i] = lanes[i];
        g_lane_pixels[i] = lanes[i].pixels;
        std::memcpy(g_order[i], kIndex[static_cast<size_t>(lanes[i].order) % 6], 3);
        g_positions = lanes[i].pixels > g_positions ? lanes[i].pixels : g_positions;
    }
    g_count = count;
    const size_t ring_bytes = kRing * kBufferSlots * sizeof(uint16_t);
    const size_t latch_bytes = kLatchSlots * sizeof(uint16_t);
    g_ring = static_cast<uint16_t *>(heap_caps_calloc(1, ring_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    g_latch = static_cast<uint16_t *>(heap_caps_calloc(1, latch_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    g_descriptors = static_cast<lldesc_t *>(
        heap_caps_calloc(kRing + 1, sizeof(lldesc_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (g_done == nullptr) {
        g_done = xSemaphoreCreateBinary();
    }
    if (g_ring == nullptr || g_latch == nullptr || g_descriptors == nullptr || g_done == nullptr) {
        ESP_LOGE(kTag, "no internal DMA memory for the ring (%u bytes)", static_cast<unsigned>(ring_bytes + latch_bytes));
        free_buffers();
        g_count = 0;
        return ESP_ERR_NO_MEM;
    }

    // the slots that never change: the first of every bit high on every lane, the third low
    uint16_t lanes_mask = 0;
    for (size_t i = 0; i < count; i++) {
        lanes_mask |= static_cast<uint16_t>(1u << i);
    }
    for (size_t bit = 0; bit < kRing * kPositions * 24; bit++) {
        g_ring[slot_index(bit * kSlotsPerBit)] = lanes_mask;
    }
    // the ring's descriptors (their length and successor set with each fill), then the latch's,
    // which ends the chain
    for (size_t k = 0; k < kRing; k++) {
        lldesc_t &d = g_descriptors[k];
        d.size = static_cast<uint32_t>(kBufferSlots * sizeof(uint16_t));
        d.length = d.size;
        d.owner = 1;
        d.eof = 1;
        d.buf = reinterpret_cast<const uint8_t *>(g_ring + k * kBufferSlots);
        d.empty = reinterpret_cast<uint32_t>(&g_descriptors[(k + 1) % kRing]);
    }
    lldesc_t &latch = g_descriptors[kRing];
    latch.size = static_cast<uint32_t>(latch_bytes);
    latch.length = latch.size;
    latch.owner = 1;
    latch.eof = 1;
    latch.buf = reinterpret_cast<const uint8_t *>(g_latch);
    latch.empty = 0;

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
    ESP_LOGI(kTag, "I2S ring of %u x %u bytes, %u LED positions", static_cast<unsigned>(kRing),
             static_cast<unsigned>(kBufferSlots * sizeof(uint16_t)), static_cast<unsigned>(g_positions));
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
    size_t total = 0;
    for (size_t lane = 0; lane < g_count; lane++) {
        total += g_lane_pixels[lane];
    }
    // the interrupt runs while the flash is busy, when the PSRAM cannot be read
    if (!esp_ptr_internal(rgb)) {
        if (g_staging_size < total * 3) {
            heap_caps_free(g_staging);
            g_staging = static_cast<uint8_t *>(heap_caps_malloc(total * 3, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
            g_staging_size = g_staging != nullptr ? total * 3 : 0;
            ESP_RETURN_ON_FALSE(g_staging != nullptr, ESP_ERR_NO_MEM, kTag, "no internal memory for the frame");
        }
        std::memcpy(g_staging, rgb, total * 3);
        rgb = g_staging;
    }
    for (size_t lane = 0; lane < g_count; lane++) {
        g_lane_rgb[lane] = rgb;
        rgb += g_lane_pixels[lane] * 3;
    }
    // the first buffers before the start, the rest from the interrupt
    g_next_position = 0;
    for (size_t k = 0; k < kRing && fill(k); k++) {
    }
    g_refill = 0;
    g_max_cycles = 0;

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
    hw->int_ena.out_eof = 1;
    hw->int_ena.out_total_eof = 1;
    hw->out_link.addr = reinterpret_cast<uint32_t>(&g_descriptors[0]) & 0xfffff;
    hw->out_link.start = 1;
    hw->conf.tx_start = 1;
    if (xSemaphoreTake(g_done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        hw->conf.tx_start = 0;
        hw->int_ena.val = 0;
        ESP_LOGW(kTag, "the I2S frame did not end");
        return ESP_ERR_TIMEOUT;
    }
    hw->int_ena.val = 0;
    if (g_late != g_late_reported) {
        ESP_LOGW(kTag, "the encoder was nearly caught up %u times", static_cast<unsigned>(g_late - g_late_reported));
        g_late_reported = g_late;
    }
    return ESP_OK;
}

uint32_t encoder_max_us()
{
    return g_max_cycles / (esp_clk_cpu_freq() / 1000000);
}

std::expected<uint32_t, esp_err_t> encoder_test()
{
    if (g_count == 0) {
        return std::unexpected(ESP_ERR_INVALID_STATE);
    }
    // one row of white for every lane: the encoder reads lane n's LED p at p * 3
    auto *white = static_cast<uint8_t *>(heap_caps_malloc(g_positions * 3, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (white == nullptr) {
        return std::unexpected(ESP_ERR_NO_MEM);
    }
    std::memset(white, 0xff, g_positions * 3);
    for (size_t lane = 0; lane < g_count; lane++) {
        g_lane_rgb[lane] = white;
    }
    // every buffer as the interrupt fills it, other interrupts off (also no move to the other core,
    // which counts its own cycles); show() sets the ring up again
    uint32_t longest = 0;
    g_next_position = 0;
    for (size_t k = 0;; k = (k + 1) % kRing) {
        portDISABLE_INTERRUPTS();
        const uint32_t start = esp_cpu_get_cycle_count();
        const bool more = fill(k);
        const uint32_t cycles = esp_cpu_get_cycle_count() - start;
        portENABLE_INTERRUPTS();
        if (!more) {
            break;
        }
        longest = cycles > longest ? cycles : longest;
    }
    heap_caps_free(white);
    return longest / (esp_clk_cpu_freq() / 1000000);
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

uint32_t encoder_max_us()
{
    return 0;
}

std::expected<uint32_t, esp_err_t> encoder_test()
{
    return std::unexpected(ESP_ERR_NOT_SUPPORTED);
}

} // namespace px::output::detail::i2s

#endif
