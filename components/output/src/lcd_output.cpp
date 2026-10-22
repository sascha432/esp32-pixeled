// The parallel backend of px::output on the ESP32-S3 (docs/leds.md section 2.2): LCD_CAM in i80
// mode sends 16-bit samples, bit n of a sample on lane n, without the WR and DC pins (the LEDs
// need no clock). A WS2812 bit is three samples (slots) at 2.42 MHz (80 MHz / 33), 0.41 us each:
// high, the data bit, low. So a 0 is 0.41 us high and 0.83 us low, a 1 is 0.83 us high and
// 0.41 us low, all lanes at once.
//
// The same ring as the ESP32's I2S backend (i2s_output.cpp): kRing small buffers in internal DMA
// memory, kPositions LED positions each, the slots that are always high or low written once. A
// GDMA channel reads them in a circle; its EOF interrupt at the end of each buffer encodes the next
// positions into it, kRing - 1 buffers (2.1 ms) ahead of the DMA. After the last positions the
// chain goes on to the latch (zeros) and ends; the LCD stops when the DMA has no more data. 14 KB
// for any LED count.
//
// The interrupt runs from IRAM, also while the flash is busy: it reads the frame from internal
// RAM (a frame in the PSRAM is copied first). esp_lcd's i80 driver is not used: it needs a WR and
// a DC pin.

#include "backends.hpp"

#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32S3

#include <cstring>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_private/esp_clk.h"
#include "esp_private/gdma.h"
#include "esp_private/periph_ctrl.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hal/dma_types.h"
#include "hal/lcd_ll.h"
#include "hal/lcd_periph.h"
#include "soc/gpio_sig_map.h"
#include "soc/lcd_cam_struct.h"

namespace px::output::detail::parallel {

namespace {

constexpr const char *kTag = "output";

constexpr size_t kSlotsPerBit = 3;
constexpr size_t kSlotsPerLed = 24 * kSlotsPerBit;
// the latch: 800 slots of 0.41 us, 330 us (the WS2812B needs 280)
constexpr size_t kLatchSlots = 800;
// the ring: 4 buffers of 24 positions, 3456 bytes and 0.71 ms each (a descriptor carries at most
// 4095 bytes)
constexpr size_t kPositions = 24;
constexpr size_t kRing = 4;
constexpr size_t kBufferSlots = kPositions * kSlotsPerLed;
// the LCD clock: PLL 160 MHz / 2, then / 33 for the slots
constexpr int kGroupDivider = 2;
constexpr uint32_t kSlotDivider = 33;

lcd_cam_dev_t *const g_hw = &LCD_CAM;
Lane g_lanes[kMaxLanes] = {};
size_t g_count = 0;
size_t g_positions = 0; // the LEDs of the longest lane
uint16_t *g_ring = nullptr; // kRing buffers of kBufferSlots
uint16_t *g_latch = nullptr;
dma_descriptor_t *g_descriptors = nullptr; // the ring's, then the latch's
gdma_channel_handle_t g_dma = nullptr;
// this backend holds the LCD_CAM bus clock
bool g_bus_clock = false;
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
            slots[b * kSlotsPerBit + 1] = words[b];
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
    dma_descriptor_t &d = g_descriptors[k];
    d.dw0.length = static_cast<uint32_t>(count * kSlotsPerLed * sizeof(uint16_t));
    d.dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
    d.next = g_next_position >= g_positions ? &g_descriptors[kRing] : &g_descriptors[(k + 1) % kRing];
    return true;
}

IRAM_ATTR bool on_eof(gdma_channel_handle_t, gdma_event_data_t *event, void *)
{
    // the descriptor the DMA read last: every ring buffer up to it is free again (two ends may
    // come with one interrupt)
    const auto *done = reinterpret_cast<const dma_descriptor_t *>(event->tx_eof_desc_addr);
    if (done == &g_descriptors[kRing]) {
        BaseType_t woken = pdFALSE;
        xSemaphoreGiveFromISR(g_done, &woken);
        return woken == pdTRUE;
    }
    if (done < g_descriptors || done >= g_descriptors + kRing) {
        return false;
    }
    const auto last = static_cast<size_t>(done - g_descriptors);
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
    return false;
}

esp_err_t configure_hardware()
{
    gdma_channel_alloc_config_t channel{};
    channel.intr_priority = 3;
    channel.flags.isr_cache_safe = 1;
    ESP_RETURN_ON_ERROR(gdma_new_ahb_channel(&channel, &g_dma, nullptr), kTag, "no DMA channel");
    ESP_RETURN_ON_ERROR(gdma_connect(g_dma, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_LCD, 0)), kTag, "DMA to LCD");
    // the descriptors are set up again for every frame: no owner check, no write back
    const gdma_strategy_config_t strategy = {.owner_check = false, .auto_update_desc = false, .eof_till_data_popped = false};
    gdma_apply_strategy(g_dma, &strategy);
    gdma_transfer_config_t transfer{};
    transfer.max_data_burst_size = 32;
    transfer.access_ext_mem = false;
    ESP_RETURN_ON_ERROR(gdma_config_transfer(g_dma, &transfer), kTag, "DMA transfer");
    gdma_tx_event_callbacks_t callbacks{};
    callbacks.on_trans_eof = on_eof;
    ESP_RETURN_ON_ERROR(gdma_register_tx_event_callbacks(g_dma, &callbacks, nullptr), kTag, "DMA interrupt");

    // the bus clock is shared with the camera; counted, so that only the first user resets it
    PERIPH_RCC_ACQUIRE_ATOMIC(soc_lcd_i80_signals[0].module, ref_count)
    {
        if (ref_count == 0) {
            lcd_ll_enable_bus_clock(0, true);
            lcd_ll_reset_register(0);
        }
    }
    g_bus_clock = true;
    PERIPH_RCC_ATOMIC()
    {
        lcd_ll_enable_clock(g_hw, true);
        lcd_ll_select_clk_src(g_hw, LCD_CLK_SRC_PLL160M);
        lcd_ll_set_group_clock_coeff(g_hw, kGroupDivider, 0, 0);
    }
    lcd_ll_set_pixel_clock_prescale(g_hw, kSlotDivider);
    lcd_ll_reset(g_hw);
    lcd_ll_fifo_reset(g_hw);
    lcd_ll_enable_rgb_mode(g_hw, false);
    lcd_ll_enable_color_convert(g_hw, false);
    lcd_ll_set_dma_read_stride(g_hw, 16);
    lcd_ll_set_data_wire_width(g_hw, 16);
    lcd_ll_reverse_dma_data_bit_order(g_hw, false);
    lcd_ll_swap_dma_data_byte_order(g_hw, false);
    lcd_ll_enable_swizzle(g_hw, false);
    // only the data phase, as long as the DMA has data
    lcd_ll_set_phase_cycles(g_hw, 0, 0, 1);
    lcd_ll_set_blank_cycles(g_hw, 0, 0);
    lcd_ll_enable_output_always_on(g_hw, true);
    lcd_ll_clear_interrupt_status(g_hw, UINT32_MAX);
    return ESP_OK;
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

void release_dma()
{
    if (g_dma != nullptr) {
        gdma_stop(g_dma);
        gdma_disconnect(g_dma);
        gdma_del_channel(g_dma);
        g_dma = nullptr;
    }
    if (g_bus_clock) {
        PERIPH_RCC_RELEASE_ATOMIC(soc_lcd_i80_signals[0].module, ref_count)
        {
            if (ref_count == 0) {
                lcd_ll_enable_bus_clock(0, false);
            }
        }
        g_bus_clock = false;
    }
}

} // namespace

bool supported()
{
    return true;
}

esp_err_t start(const Lane *lanes, size_t count)
{
    stop();
    ESP_RETURN_ON_FALSE(count <= kMaxLanes, ESP_ERR_NOT_SUPPORTED, kTag, "%u lanes, LCD_CAM has 16",
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
    g_descriptors = static_cast<dma_descriptor_t *>(
        heap_caps_calloc(kRing + 1, sizeof(dma_descriptor_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
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
        g_ring[bit * kSlotsPerBit] = lanes_mask;
    }
    // the ring's descriptors (their length and successor set with each fill), then the latch's,
    // which ends the chain; each one ends with an EOF, the interrupt of the refill
    for (size_t k = 0; k < kRing; k++) {
        dma_descriptor_t &d = g_descriptors[k];
        d.dw0.size = kBufferSlots * sizeof(uint16_t);
        d.dw0.length = d.dw0.size;
        d.dw0.suc_eof = 1;
        d.dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
        d.buffer = g_ring + k * kBufferSlots;
        d.next = &g_descriptors[(k + 1) % kRing];
    }
    dma_descriptor_t &latch = g_descriptors[kRing];
    latch.dw0.size = latch_bytes;
    latch.dw0.length = latch_bytes;
    latch.dw0.suc_eof = 1;
    latch.dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
    latch.buffer = g_latch;
    latch.next = nullptr;

    const esp_err_t err = configure_hardware();
    if (err != ESP_OK) {
        release_dma();
        free_buffers();
        g_count = 0;
        return err;
    }
    for (size_t i = 0; i < count; i++) {
        const auto pin = static_cast<gpio_num_t>(lanes[i].gpio);
        gpio_set_level(pin, 0);
        gpio_set_direction(pin, GPIO_MODE_OUTPUT);
        esp_rom_gpio_pad_select_gpio(pin);
        esp_rom_gpio_connect_out_signal(pin, soc_lcd_i80_signals[0].data_sigs[i], false, false);
        ESP_LOGI(kTag, "lane %u on GPIO %d, %u pixels (LCD_CAM)", static_cast<unsigned>(i + 1), lanes[i].gpio,
                 lanes[i].pixels);
    }
    ESP_LOGI(kTag, "LCD_CAM ring of %u x %u bytes, %u LED positions", static_cast<unsigned>(kRing),
             static_cast<unsigned>(kBufferSlots * sizeof(uint16_t)), static_cast<unsigned>(g_positions));
    return ESP_OK;
}

void stop()
{
    if (g_count == 0) {
        return;
    }
    lcd_ll_stop(g_hw);
    release_dma();
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
    lcd_ll_stop(g_hw);
    lcd_ll_reset(g_hw);
    lcd_ll_fifo_reset(g_hw);
    lcd_ll_clear_interrupt_status(g_hw, UINT32_MAX);
    gdma_reset(g_dma);
    gdma_start(g_dma, reinterpret_cast<intptr_t>(&g_descriptors[0]));
    // the DMA needs a moment to put the first samples into the LCD's FIFO (esp_lcd waits as long)
    esp_rom_delay_us(4);
    lcd_ll_start(g_hw);
    if (xSemaphoreTake(g_done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        lcd_ll_stop(g_hw);
        gdma_stop(g_dma);
        ESP_LOGW(kTag, "the LCD_CAM frame did not end");
        return ESP_ERR_TIMEOUT;
    }
    // the DMA read the latch; the LCD sends the last of it from its FIFO (16 samples, 7 us)
    for (uint32_t spins = 0; g_hw->lc_dma_int_raw.lcd_trans_done_int_raw == 0 && spins < 100000; spins++) {
    }
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
    // every buffer as the interrupt fills it, other interrupts off; show() sets the ring up again
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

} // namespace px::output::detail::parallel

#endif
