// Buttons and the rotary encoder (px/input/input.hpp, docs/leds.md section 9).

#include "px/input/input.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <iterator>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "px/platform/settings.hpp"
#include "soc/soc_caps.h"

#if SOC_PCNT_SUPPORTED
#include "driver/pulse_cnt.h"
#endif

#include "button.hpp"

namespace px::input {

namespace {

constexpr const char *kTag = "input";
constexpr const char *kNamespace = "input";
constexpr const char *kKeySettings = "settings";
constexpr uint64_t kSampleUs = 10 * 1000;
// a hold of the restart action
constexpr uint32_t kRestartHoldMs = 3000;
// the encoder: counts per detent (both edges of both pins), and how long the knob stays on the
// animations after it last moved
constexpr int kCountsPerDetent = 4;
constexpr uint32_t kKnobMs = 10000;

struct Named {
    const char *slug;
    const char *name;
};

constexpr Named kActions[kActionCount] = {
    {"none", "Nothing"},
    {"on-off", "On / off"},
    {"next-animation", "Next animation"},
    {"previous-animation", "Previous animation"},
    {"next-preset", "Next preset"},
    {"brightness-up", "Brighter"},
    {"brightness-down", "Darker"},
    {"restart", "Restart (held 3 s)"},
    {"hotspot", "Open the hotspot"},
    {"knob", "Knob: animations"},
};

SemaphoreHandle_t g_lock = nullptr;
// under g_lock: the timer reads them, the web server's task sets them
Settings g_settings = {};
Status g_status = {};

// the timer's task only
Handler g_handler = nullptr;
int8_t g_pins[kMaxButtons + 1] = {-1, -1, -1, -1, -1};
detail::Button *g_buttons[kMaxButtons + 1] = {};
esp_timer_handle_t g_timer = nullptr;
int g_counts_used = 0;
int64_t g_last_detent_us = 0;
int64_t g_knob_until_us = 0;
int32_t g_position = 0;

#if SOC_PCNT_SUPPORTED
pcnt_unit_handle_t g_pcnt = nullptr;
#else
// the C3: both pins' edges in an interrupt, the quarter steps of the Gray code counted
std::atomic<int32_t> g_isr_count{0};
uint8_t g_isr_state = 0;
#endif
int8_t g_encoder_a = -1;
int8_t g_encoder_b = -1;

detail::Behavior behavior_of(const Actions &a)
{
    const bool repeat = a.hold == Action::BrightnessUp || a.hold == Action::BrightnessDown;
    // without a hold action any press is a click, however long
    const uint32_t hold_ms = a.hold == Action::None      ? UINT32_MAX
                             : a.hold == Action::Restart ? kRestartHoldMs
                                                         : detail::kHoldMs;
    return {.double_click = a.double_click != Action::None, .hold_ms = hold_ms, .repeat = repeat};
}

void run(Action action, int steps)
{
    if (action == Action::None) {
        return;
    }
    if (action == Action::Knob) {
        g_knob_until_us = esp_timer_get_time() + static_cast<int64_t>(kKnobMs) * 1000;
        ESP_LOGI(kTag, "the knob changes the animation");
        return;
    }
    ESP_LOGI(kTag, "%s", action_slug(action));
    g_handler(action, steps);
}

// the encoder's count since the start, in its counts
int encoder_count()
{
#if SOC_PCNT_SUPPORTED
    int count = 0;
    if (g_pcnt != nullptr) {
        pcnt_unit_get_count(g_pcnt, &count);
    }
    return count;
#else
    return static_cast<int>(g_isr_count.load());
#endif
}

void on_sample(void *)
{
    const auto now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    xSemaphoreTake(g_lock, portMAX_DELAY);
    const Settings settings = g_settings;
    xSemaphoreGive(g_lock);
    bool pressed[kMaxButtons + 1] = {};
    for (size_t i = 0; i <= kMaxButtons; i++) {
        if (g_buttons[i] == nullptr) {
            continue;
        }
        const Actions &a = settings.button[i];
        const detail::Event event = g_buttons[i]->update(gpio_get_level(static_cast<gpio_num_t>(g_pins[i])) == 0, now_ms,
                                                         behavior_of(a));
        pressed[i] = g_buttons[i]->pressed();
        switch (event) {
        case detail::Event::Click:
            run(a.click, 1);
            break;
        case detail::Event::DoubleClick:
            run(a.double_click, 1);
            break;
        case detail::Event::Hold:
            run(a.hold, 1);
            break;
        case detail::Event::None:
            break;
        }
    }
    // the encoder: whole detents since the last sample
    const int64_t now_us = esp_timer_get_time();
    if (g_encoder_a >= 0) {
        const int count = encoder_count();
        const int detents = (count - g_counts_used) / kCountsPerDetent;
        if (detents != 0) {
            g_counts_used += detents * kCountsPerDetent;
            g_position += detents;
            const auto gap_ms = static_cast<uint32_t>((now_us - g_last_detent_us) / 1000);
            g_last_detent_us = now_us;
            if (now_us < g_knob_until_us) {
                // one animation per detent, no acceleration; the knob stays on them while it moves
                g_knob_until_us = now_us + static_cast<int64_t>(kKnobMs) * 1000;
                g_handler(detents > 0 ? Action::NextAnimation : Action::PreviousAnimation, detents > 0 ? detents : -detents);
            } else {
                const int steps = detail::accelerate(detents, gap_ms);
                g_handler(steps > 0 ? Action::BrightnessUp : Action::BrightnessDown, steps > 0 ? steps : -steps);
            }
        }
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    std::memcpy(g_status.pressed, pressed, sizeof(pressed));
    g_status.encoder_position = g_position;
    g_status.knob_on_animations = now_us < g_knob_until_us;
    xSemaphoreGive(g_lock);
}

// a button's pin: an input with the pull-up where the pin has one
esp_err_t setup_pin(int8_t gpio)
{
    gpio_config_t config{};
    config.pin_bit_mask = 1ULL << gpio;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_IS_VALID_OUTPUT_GPIO(gpio) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
    return gpio_config(&config);
}

#if SOC_PCNT_SUPPORTED

esp_err_t start_encoder(int8_t a, int8_t b)
{
    pcnt_unit_config_t unit{};
    unit.low_limit = -1000;
    unit.high_limit = 1000;
    // the count goes on past the limits (the watch points below)
    unit.flags.accum_count = 1;
    ESP_RETURN_ON_ERROR(pcnt_new_unit(&unit, &g_pcnt), kTag, "pcnt unit");
    // contact bounce shorter than 1 us is not counted
    pcnt_glitch_filter_config_t filter{};
    filter.max_glitch_ns = 1000;
    ESP_RETURN_ON_ERROR(pcnt_unit_set_glitch_filter(g_pcnt, &filter), kTag, "filter");
    // the quadrature in full: each pin's edges count up or down by the other pin's level
    pcnt_chan_config_t chan_a{};
    chan_a.edge_gpio_num = a;
    chan_a.level_gpio_num = b;
    pcnt_channel_handle_t channel_a = nullptr;
    ESP_RETURN_ON_ERROR(pcnt_new_channel(g_pcnt, &chan_a, &channel_a), kTag, "channel a");
    pcnt_chan_config_t chan_b{};
    chan_b.edge_gpio_num = b;
    chan_b.level_gpio_num = a;
    pcnt_channel_handle_t channel_b = nullptr;
    ESP_RETURN_ON_ERROR(pcnt_new_channel(g_pcnt, &chan_b, &channel_b), kTag, "channel b");
    pcnt_channel_set_edge_action(channel_a, PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    pcnt_channel_set_level_action(channel_a, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    pcnt_channel_set_edge_action(channel_b, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE);
    pcnt_channel_set_level_action(channel_b, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    pcnt_unit_add_watch_point(g_pcnt, unit.low_limit);
    pcnt_unit_add_watch_point(g_pcnt, unit.high_limit);
    // the encoder pulls its pins to GND: pull-ups (after the channels took the pins)
    gpio_set_pull_mode(static_cast<gpio_num_t>(a), GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(static_cast<gpio_num_t>(b), GPIO_PULLUP_ONLY);
    ESP_RETURN_ON_ERROR(pcnt_unit_enable(g_pcnt), kTag, "enable");
    ESP_RETURN_ON_ERROR(pcnt_unit_clear_count(g_pcnt), kTag, "clear");
    return pcnt_unit_start(g_pcnt);
}

#else

// the Gray code's step from the last two pin levels to the new ones: +1, -1, or 0 (no change, or a
// bounce that skipped a state)
constexpr int8_t kQuadrature[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};

void IRAM_ATTR on_encoder_edge(void *)
{
    const auto levels = static_cast<uint8_t>(gpio_get_level(static_cast<gpio_num_t>(g_encoder_a)) << 1 |
                                             gpio_get_level(static_cast<gpio_num_t>(g_encoder_b)));
    g_isr_state = static_cast<uint8_t>((g_isr_state << 2 | levels) & 15);
    g_isr_count.fetch_add(kQuadrature[g_isr_state], std::memory_order_relaxed);
}

esp_err_t start_encoder(int8_t a, int8_t b)
{
    gpio_config_t config{};
    config.pin_bit_mask = (1ULL << a) | (1ULL << b);
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    config.intr_type = GPIO_INTR_ANYEDGE;
    ESP_RETURN_ON_ERROR(gpio_config(&config), kTag, "encoder pins");
    // another component may have installed the service already
    const esp_err_t err = gpio_install_isr_service(0);
    ESP_RETURN_ON_FALSE(err == ESP_OK || err == ESP_ERR_INVALID_STATE, err, kTag, "isr service");
    g_isr_state = static_cast<uint8_t>(gpio_get_level(static_cast<gpio_num_t>(a)) << 1 |
                                       gpio_get_level(static_cast<gpio_num_t>(b)));
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(static_cast<gpio_num_t>(a), on_encoder_edge, nullptr), kTag, "isr a");
    return gpio_isr_handler_add(static_cast<gpio_num_t>(b), on_encoder_edge, nullptr);
}

#endif

bool valid(const Settings &s, char *error, size_t error_size)
{
    for (size_t i = 0; i <= kMaxButtons; i++) {
        const Actions &a = s.button[i];
        if (static_cast<size_t>(a.click) >= kActionCount || static_cast<size_t>(a.double_click) >= kActionCount ||
            static_cast<size_t>(a.hold) >= kActionCount) {
            std::snprintf(error, error_size, "button %u: not an action", static_cast<unsigned>(i + 1));
            return false;
        }
        // a restart only on purpose
        if (a.click == Action::Restart || a.double_click == Action::Restart) {
            std::snprintf(error, error_size, "restart only as a hold (3 s)");
            return false;
        }
    }
    return true;
}

} // namespace

Settings defaults()
{
    Settings s{};
    s.button[0] = {Action::NextAnimation, Action::None, Action::Restart};
    s.button[1] = {Action::BrightnessDown, Action::None, Action::BrightnessDown};
    s.button[2] = {Action::BrightnessUp, Action::None, Action::BrightnessUp};
    s.button[3] = {Action::OnOff, Action::None, Action::NextPreset};
    s.button[kEncoderButton] = {Action::OnOff, Action::NextPreset, Action::Knob};
    return s;
}

const char *action_slug(Action action)
{
    const auto i = static_cast<size_t>(action);
    return i < kActionCount ? kActions[i].slug : "none";
}

const char *action_name(Action action)
{
    const auto i = static_cast<size_t>(action);
    return i < kActionCount ? kActions[i].name : "";
}

bool find_action(const char *slug, Action &out)
{
    for (size_t i = 0; i < kActionCount; i++) {
        if (std::strcmp(kActions[i].slug, slug) == 0) {
            out = static_cast<Action>(i);
            return true;
        }
    }
    return false;
}

bool held_at_boot(int8_t gpio, uint32_t ms)
{
    if (gpio < 0 || setup_pin(gpio) != ESP_OK) {
        return false;
    }
    // the pull-up needs a moment before the first read
    vTaskDelay(pdMS_TO_TICKS(2));
    for (uint32_t waited = 0;; waited += 10) {
        if (gpio_get_level(static_cast<gpio_num_t>(gpio)) != 0) {
            return false;
        }
        if (waited >= ms) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t start(const Pins &pins, Handler handler)
{
    ESP_RETURN_ON_FALSE(g_lock == nullptr, ESP_ERR_INVALID_STATE, kTag, "already started");
    ESP_RETURN_ON_FALSE(handler != nullptr, ESP_ERR_INVALID_ARG, kTag, "handler");
    bool any = pins.encoder_a >= 0 && pins.encoder_b >= 0;
    for (size_t i = 0; i < pins.button_count && i < kMaxButtons; i++) {
        g_pins[i] = pins.buttons[i];
        any = any || g_pins[i] >= 0;
    }
    g_pins[kEncoderButton] = pins.encoder_button;
    any = any || pins.encoder_button >= 0;
    if (!any) {
        return ESP_ERR_NOT_FOUND;
    }
    g_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(g_lock != nullptr, ESP_ERR_NO_MEM, kTag, "lock");
    g_handler = handler;
    g_settings = defaults();
    {
        platform::Settings nvs(kNamespace, platform::Settings::Mode::Read);
        Settings stored{};
        size_t size = sizeof(stored);
        char error[64];
        if (nvs.ok() && nvs.get_blob(kKeySettings, &stored, size) == ESP_OK && size == sizeof(stored) &&
            stored.version == Settings{}.version && valid(stored, error, sizeof(error))) {
            g_settings = stored;
        }
    }
    for (size_t i = 0; i <= kMaxButtons; i++) {
        if (g_pins[i] < 0) {
            continue;
        }
        if (setup_pin(g_pins[i]) != ESP_OK) {
            ESP_LOGW(kTag, "button %u (GPIO %d) did not start", static_cast<unsigned>(i + 1), g_pins[i]);
            g_pins[i] = -1;
            continue;
        }
        // a button still held from the boot (the hotspot) counts from its release on
        g_buttons[i] = new detail::Button(gpio_get_level(static_cast<gpio_num_t>(g_pins[i])) == 0);
    }
    if (pins.encoder_a >= 0 && pins.encoder_b >= 0) {
        if (start_encoder(pins.encoder_a, pins.encoder_b) == ESP_OK) {
            g_encoder_a = pins.encoder_a;
            g_encoder_b = pins.encoder_b;
            g_counts_used = encoder_count();
        } else {
            ESP_LOGW(kTag, "the encoder (GPIO %d, %d) did not start", pins.encoder_a, pins.encoder_b);
        }
    }
    g_status.running = true;
    for (size_t i = 0; i < kMaxButtons; i++) {
        g_status.button_gpio[i] = g_pins[i];
    }
    g_status.encoder_a = g_encoder_a;
    g_status.encoder_b = g_encoder_b;
    g_status.encoder_button = g_pins[kEncoderButton];
    g_status.hotspot_button = pins.hotspot_button;
    esp_timer_create_args_t timer{};
    timer.callback = on_sample;
    timer.name = "input";
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer, &g_timer), kTag, "timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(g_timer, kSampleUs), kTag, "timer");
    ESP_LOGI(kTag, "buttons on GPIO %d, %d, %d, %d; encoder %d, %d, push %d", g_pins[0], g_pins[1], g_pins[2],
             g_pins[3], g_encoder_a, g_encoder_b, g_pins[kEncoderButton]);
    return ESP_OK;
}

bool running()
{
    return g_lock != nullptr;
}

Settings settings()
{
    if (g_lock == nullptr) {
        return defaults();
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    const Settings out = g_settings;
    xSemaphoreGive(g_lock);
    return out;
}

bool set_settings(const Settings &in, char *error, size_t error_size)
{
    if (g_lock == nullptr) {
        std::snprintf(error, error_size, "this device has no buttons");
        return false;
    }
    Settings s = in;
    s.version = Settings{}.version;
    if (!valid(s, error, error_size)) {
        return false;
    }
    platform::Settings nvs(kNamespace, platform::Settings::Mode::ReadWrite);
    if (!nvs.ok() || nvs.set_blob(kKeySettings, &s, sizeof(s)) != ESP_OK || nvs.commit() != ESP_OK) {
        std::snprintf(error, error_size, "the settings could not be stored");
        return false;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_settings = s;
    xSemaphoreGive(g_lock);
    return true;
}

Status status()
{
    if (g_lock == nullptr) {
        Status out{};
        std::memset(out.button_gpio, -1, sizeof(out.button_gpio));
        out.encoder_a = out.encoder_b = out.encoder_button = -1;
        return out;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    const Status out = g_status;
    xSemaphoreGive(g_lock);
    return out;
}

} // namespace px::input
