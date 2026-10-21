// The PixeLED firmware: composition root. It creates and wires the components and contains no
// logic of its own (docs/architecture.md section 3).

#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_rom_sys.h"
#include "px/audio/audio.hpp"
#include "px/board/board.hpp"
#include "px/devconsole/devconsole.hpp"
#include "px/leds/leds.hpp"
#include "px/leds_api/leds_api.hpp"
#include "px/light_sensor/ldr.hpp"
#include "px/light_sensor/light_sensor.hpp"
#include "px/logbuf/logbuf.hpp"
#include "px/memory/large.hpp"
#include "px/motion/motion.hpp"
#include "px/hass/hass.hpp"
#include "px/network/network.hpp"
#include "px/platform/device.hpp"
#include "px/platform/storage.hpp"
#include "px/platform/system_info.hpp"
#include "px/platform/time_sync.hpp"
#include "px/relay/relay.hpp"
#include "px/webserver/webserver.hpp"
#include "backup.hpp"

namespace {

constexpr const char *kTag = "main";

// Every failed allocation with the state of both heaps, also with the log output off.
void on_alloc_failed(size_t size, uint32_t caps, const char *function)
{
    esp_rom_printf("alloc failed: %u bytes, caps 0x%x, %s; internal %u free (largest %u), PSRAM %u free "
                   "(largest %u)\n",
                   static_cast<unsigned>(size), static_cast<unsigned>(caps), function,
                   static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                   static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                   static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                   static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
}

// The optional hardware of the board (docs/architecture.md section 8.1): what is not fitted or
// does not answer is logged and left out.
void start_hardware(const px::board::Info &board, const px::board::Handles &handles)
{
    // The LED power supply stays off until the LEDs show something (docs/leds.md section 5):
    // relay 1 the supply, relay 2 its indicator LED.
    if (board.power_gpio >= 0 || board.power_led_gpio >= 0) {
        const int8_t gpios[] = {board.power_gpio, board.power_led_gpio};
        const bool pullups[] = {false, false};
        const bool active_low[] = {!board.power_active_high, false};
        if (px::relay::start(gpios, pullups, active_low, 2) != ESP_OK) {
            ESP_LOGW(kTag, "LED power switch did not start");
        }
        // a crash leaves the LEDs as they were until the restart: the supply goes off first
        px::relay::off_at_crash(1);
        px::relay::off_at_crash(2);
    }
    const bool bh1750 = handles.i2c != nullptr &&
                        px::light_sensor::start(handles.i2c, board.light_sensor_scale) == ESP_OK;
    if (!bh1750 && (board.light_ldr_gpio < 0 || px::light_sensor::ldr::start(board.light_ldr_gpio) != ESP_OK)) {
        ESP_LOGI(kTag, "no light sensor");
    }
    if (board.motion_gpio >= 0 &&
        px::motion::start(board.motion_gpio, board.motion_active_high,
                          [](bool detected) {
                              ESP_LOGI(kTag, "motion %s", detected ? "detected" : "clear");
                              px::leds::motion(detected);
                              px::hass::publish_motion(detected);
                          }) !=
            ESP_OK) {
        ESP_LOGW(kTag, "motion sensor did not start");
    }
    // the audio input: the microphone where the board has one, the UDP inputs on every board
    if (px::audio::start({.sck = board.mic_sck_gpio, .ws = board.mic_ws_gpio, .sd = board.mic_sd_gpio}) != ESP_OK) {
        ESP_LOGW(kTag, "audio input did not start");
    }
}

// The render task (docs/leds.md section 3) on the board's LED pins; the supply and its indicator
// LED are relays 1 and 2.
void start_leds(const px::board::Info &board)
{
    const px::leds::Start start{
        .lane_gpios = board.led_gpios.data(),
        .lane_count = board.led_gpios.size(),
        .supply =
            [](bool on) {
                px::relay::set(1, on);
                px::relay::set(2, on);
            },
        .has_motion = px::motion::running(),
        .set_motion_hold = px::motion::running() ? px::motion::set_hold_s : nullptr,
        .motion_hold_left = px::motion::running() ? px::motion::hold_left_s : nullptr,
        .audio = px::audio::running() ? px::audio::frame : nullptr,
        .has_audio = px::audio::running() ? px::audio::has_source : nullptr,
    };
    if (px::leds::start(start) != ESP_OK) {
        ESP_LOGE(kTag, "the LEDs did not start");
    }
    // the sensor started before the LEDs: they get its state now, later every change
    if (px::motion::running()) {
        px::leds::motion(px::motion::detected());
    }
}

} // namespace

extern "C" void app_main()
{
    // first, so the web UI's log has the boot as well
    px::logbuf::start();
    heap_caps_register_failed_alloc_callback(on_alloc_failed);
    // large data (the backup's trees, later the frame buffers) prefers the PSRAM where there is one
    // (docs/architecture.md section 11)
    px::memory::set_large_heap(
        [](size_t size) {
            return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
        },
        [](void *pointer) { heap_caps_free(pointer); });
    px::platform::log_system_info();
    ESP_ERROR_CHECK(px::platform::init_storage());
    px::logbuf::restore();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
#if CONFIG_PX_DEV_CONSOLE
    // early, while the internal RAM is in one piece; it takes commands after ready() below
    if (px::devconsole::start() != ESP_OK) {
        ESP_LOGW(kTag, "development console did not start");
    }
#endif

    const auto &board = px::board::info();
    ESP_LOGI(kTag, "board %s%s%s", board.name, board.variant != nullptr ? " variant " : "",
             board.variant != nullptr ? board.variant : "");
    px::board::Handles handles{};
    px::board::init(handles);
    start_hardware(board, handles);
    start_leds(board);

    if (px::network::start(px::platform::device_name(), nullptr, nullptr) != ESP_OK) {
        ESP_LOGE(kTag, "network did not start");
    } else {
        px::platform::start_time_sync();
        if (px::audio::running() && px::audio::start_udp() != ESP_OK) {
            ESP_LOGW(kTag, "the UDP audio inputs did not start");
        }
        if (px::webserver::start({.device_name = px::platform::device_name(),
                                  .backup = &backup_access(),
                                  .add_routes =
                                      [](void *server) {
                                          // status(), not frame(): the beat a frame takes is the
                                          // render task's
                                          px::leds_api::register_routes(
                                              server, px::audio::running() ? +[](px::effects::Audio &out) {
                                                  out = px::audio::status().audio;
                                              } : nullptr);
                                          px::hass::register_routes(server);
                                          if (px::audio::running()) {
                                              px::audio::register_routes(server);
                                          }
                                      }}) != ESP_OK) {
            ESP_LOGE(kTag, "web server did not start");
        }
        const px::hass::Device device{.name = px::platform::device_name(),
                                      .board = board.name,
                                      .firmware = esp_app_get_description()->version,
                                      .has_motion = board.motion_gpio >= 0,
                                      .has_audio = px::audio::running() ? px::audio::has_source : nullptr};
        if (px::hass::start(device) != ESP_OK) {
            ESP_LOGE(kTag, "MQTT did not start");
        }
    }

    // After an update the bootloader only keeps this firmware if it confirms itself: until then a
    // reset or a power loss starts the recovery app. It confirms as soon as the network and the
    // web server run, so an update can be repeated from here (docs/ui/web/web-app.md section 5).
    const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "could not confirm the firmware: %s", esp_err_to_name(err));
    }

#if CONFIG_PX_DEV_CONSOLE
    px::devconsole::ready();
#endif
}
