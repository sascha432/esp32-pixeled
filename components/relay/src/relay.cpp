#include "px/relay/relay.hpp"

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "hal/gpio_ll.h"

namespace px::relay {

namespace {

constexpr const char *kTag = "relay";

// the GPIO of relay n+1, -1: none; written once by start() before the tasks that call set()
int g_gpio[kMaxRelays] = {-1, -1, -1, -1};
// relay n+1 is on while its pin is low
bool g_active_low[kMaxRelays] = {};
// relay n+1 goes off in the panic handler (off_at_crash)
bool g_off_at_crash[kMaxRelays] = {};

int level(size_t n, bool on)
{
    return on != g_active_low[n] ? 1 : 0;
}

} // namespace

esp_err_t start(const signed char *gpios, const bool *pullups, const bool *active_low, size_t count)
{
    esp_err_t result = ESP_OK;
    for (size_t n = 0; n < kMaxRelays && n < count; n++) {
        if (gpios[n] < 0) {
            continue;
        }
        const int gpio = gpios[n];
        if (!GPIO_IS_VALID_OUTPUT_GPIO(gpio)) {
            ESP_LOGW(kTag, "relay %u: GPIO %d cannot be an output", static_cast<unsigned>(n + 1), gpio);
            result = ESP_ERR_INVALID_ARG;
            continue;
        }
        // the off level before it becomes an output: the relay must not switch while the board
        // starts. A relay on the pullup is closed by default: released (high) until it is switched.
        const bool pullup = pullups[n];
        g_active_low[n] = !pullup && active_low[n];
        gpio_set_level(static_cast<gpio_num_t>(gpio), pullup ? 1 : level(n, false));
        gpio_config_t io{};
        io.pin_bit_mask = 1ULL << gpio;
        // input too, so is_on() reads what the pin really drives; on the pullup only low is driven
        io.mode = pullup ? GPIO_MODE_INPUT_OUTPUT_OD : GPIO_MODE_INPUT_OUTPUT;
        io.pull_up_en = pullup ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
        if (gpio_config(&io) != ESP_OK) {
            ESP_LOGW(kTag, "relay %u: GPIO %d did not configure", static_cast<unsigned>(n + 1), gpio);
            result = ESP_FAIL;
            continue;
        }
        g_gpio[n] = gpio;
        ESP_LOGI(kTag, "relay %u on GPIO %d%s", static_cast<unsigned>(n + 1), gpio,
                 pullup ? ", pullup" : g_active_low[n] ? ", active low" : "");
    }
    return result;
}

bool running()
{
    for (const int gpio : g_gpio) {
        if (gpio >= 0) {
            return true;
        }
    }
    return false;
}

int gpio_of(unsigned relay)
{
    return relay >= 1 && relay <= kMaxRelays ? g_gpio[relay - 1] : -1;
}

bool set(unsigned relay, bool on)
{
    const int gpio = gpio_of(relay);
    if (gpio < 0) {
        return false;
    }
    gpio_set_level(static_cast<gpio_num_t>(gpio), level(relay - 1, on));
    return true;
}

void off_at_crash(unsigned relay)
{
    if (relay >= 1 && relay <= kMaxRelays) {
        g_off_at_crash[relay - 1] = true;
    }
}

bool is_on(unsigned relay)
{
    const int gpio = gpio_of(relay);
    return gpio >= 0 && (gpio_get_level(static_cast<gpio_num_t>(gpio)) != 0) != g_active_low[relay - 1];
}

} // namespace px::relay

// The panic handler, wrapped (CMakeLists.txt: -Wl,--wrap=esp_panic_handler, as ESP-IDF's own
// Bluetooth controller does): the relays of off_at_crash go off first, then the handler prints and
// restarts as before. From IRAM with the registers: the cache may be off, no driver call.
extern "C" void __real_esp_panic_handler(void *info);

extern "C" IRAM_ATTR void __wrap_esp_panic_handler(void *info)
{
    for (size_t n = 0; n < px::relay::kMaxRelays; n++) {
        if (px::relay::g_off_at_crash[n] && px::relay::g_gpio[n] >= 0) {
            gpio_ll_set_level(&GPIO, static_cast<uint32_t>(px::relay::g_gpio[n]), px::relay::g_active_low[n] ? 1 : 0);
        }
    }
    __real_esp_panic_handler(info);
}
