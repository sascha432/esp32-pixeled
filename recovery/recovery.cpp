// The recovery app: runs from the factory partition when the main firmware is being updated on a
// 4 MB board, is broken, or the user asks for it (docs/architecture.md section 9.5).
//
// Same storage, Wi-Fi, hotspot and web server (with its login) as the main firmware, from the
// same NVS: the web page offers the firmware and file system update and "Start the firmware".
// It drives no LEDs: the LED power switch is held off, and the state goes to the log (serial port
// and the web page's log) whenever it changes.

#include <cstdio>
#include <cstring>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "px/board/board.hpp"
#include "px/logbuf/logbuf.hpp"
#include "px/network/network.hpp"
#include "px/ota/ota.hpp"
#include "px/platform/device.hpp"
#include "px/platform/storage.hpp"
#include "px/platform/system_info.hpp"
#include "px/webserver/webserver.hpp"

namespace {

constexpr const char *kTag = "recovery";

// The LEDs stay dark: their supply off, as the main firmware leaves it at boot
void hold_led_power_off()
{
    const px::board::Info &board = px::board::info();
    if (board.power_gpio < 0) {
        return;
    }
    const auto pin = static_cast<gpio_num_t>(board.power_gpio);
    gpio_set_level(pin, board.power_active_high ? 0 : 1);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
}

// One line for the network and the update, logged when it changed
void log_state()
{
    static char last[192];
    const px::network::Status net = px::network::status();
    const px::ota::Status update = px::ota::status();
    char line[sizeof(last)];
    int n = std::snprintf(line, sizeof(line), "Wi-Fi %s %s %s, hotspot %s", px::network::state_name(net.state),
                          net.ssid, net.ip, net.hotspot_ssid);
    if (update.state != px::ota::State::Idle && n > 0 && n < static_cast<int>(sizeof(line))) {
        std::snprintf(line + n, sizeof(line) - n, ", update %s %u KB %s", px::ota::state_name(update.state),
                      static_cast<unsigned>(update.done / 1024), update.error);
    }
    if (std::strcmp(line, last) != 0) {
        std::strcpy(last, line);
        ESP_LOGI(kTag, "%s", line);
    }
}

} // namespace

extern "C" void app_main()
{
    hold_led_power_off();
    // first, so the web UI's log has the boot as well
    px::logbuf::start();
    px::platform::log_system_info();
    const px::ota::Info ota = px::ota::info();
    ESP_LOGI(kTag, "recovery app on %s, firmware in flash: %s%s", px::board::info().name,
             ota.firmware_present ? ota.firmware_version : "none",
             ota.firmware_failed ? " (did not start, rolled back)" : "");

    ESP_ERROR_CHECK(px::platform::init_storage());
    px::logbuf::restore();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    if (px::network::start(px::platform::device_name(), nullptr, nullptr) != ESP_OK) {
        ESP_LOGE(kTag, "network did not start");
        return;
    }
    if (px::webserver::start({.device_name = px::platform::device_name()}) != ESP_OK) {
        ESP_LOGE(kTag, "web server did not start");
    }

    // the hotspot is always on here (not stored, the main firmware keeps its own setting)
    px::network::force_hotspot(true);
    for (;;) {
        log_state();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
