// The UDP audio inputs (udp.hpp, docs/leds.md section 6.1): one task, a socket per input switched
// on, select() over both.

#include "udp.hpp"

#include <cstring>
#include <initializer_list>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "px/platform/settings.hpp"

#include "packets.hpp"

namespace px::audio::detail {

namespace {

constexpr const char *kTag = "audio";
constexpr const char *kNamespace = "audio";
constexpr const char *kKeyInputs = "inputs";

SemaphoreHandle_t g_lock = nullptr;
// under g_lock
Inputs g_inputs = {};
uint32_t g_changes = 0;
effects::Audio g_audio = {};
Source g_source = Source::None;
int64_t g_until_us = 0;
bool g_beat_pending = false;
uint32_t g_packets = 0;

void load_inputs()
{
    platform::Settings nvs(kNamespace, platform::Settings::Mode::Read);
    Inputs stored{};
    size_t size = sizeof(stored);
    if (nvs.ok() && nvs.get_blob(kKeyInputs, &stored, size) == ESP_OK && size == sizeof(stored) &&
        stored.version == Inputs{}.version && stored.warls_port != 0) {
        g_inputs = stored;
    }
}

int open_socket(uint16_t port)
{
    const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        return -1;
    }
    const int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        ESP_LOGW(kTag, "UDP port %u: bind failed (%d)", port, errno);
        close(fd);
        return -1;
    }
    return fd;
}

// joins (or leaves) the WLED sync group on the station's interface; fails without a network
bool membership(int fd, bool join)
{
    ip_mreq request{};
    request.imr_multiaddr.s_addr = htonl(kWledSyncGroup);
    request.imr_interface.s_addr = htonl(INADDR_ANY);
    return setsockopt(fd, IPPROTO_IP, join ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP, &request, sizeof(request)) == 0;
}

void take(const Received &received, Source source, BeatFinder &beats)
{
    effects::Audio audio = received.audio;
    const auto ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    if (!received.has_beat) {
        audio.beat = beats.step(audio, settings().beat_ratio_tenths, ms);
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_audio = audio;
    g_source = source;
    g_until_us = esp_timer_get_time() + static_cast<int64_t>(received.hold_ms) * 1000;
    // a beat stays until a frame took it
    g_beat_pending = g_beat_pending || audio.beat;
    g_packets++;
    xSemaphoreGive(g_lock);
}

void task(void *)
{
    int warls = -1;
    int sync = -1;
    bool joined = false;
    int64_t sync_seen_us = 0;
    int64_t join_tried_us = 0;
    uint32_t applied = ~0u;
    BeatFinder beats;
    static uint8_t buffer[1024];
    for (;;) {
        xSemaphoreTake(g_lock, portMAX_DELAY);
        const Inputs inputs = g_inputs;
        const uint32_t changes = g_changes;
        xSemaphoreGive(g_lock);
        if (changes != applied) {
            applied = changes;
            if (warls >= 0) {
                close(warls);
                warls = -1;
            }
            if (sync >= 0) {
                close(sync);
                sync = -1;
                joined = false;
            }
            warls = inputs.warls ? open_socket(inputs.warls_port) : -1;
            sync = inputs.wled_sync ? open_socket(kWledSyncPort) : -1;
            if (warls >= 0) {
                ESP_LOGI(kTag, "WARLS on UDP port %u", inputs.warls_port);
            }
            join_tried_us = 0;
        }
        const int64_t now = esp_timer_get_time();
        // the group is joined once there is a network, and joined again when nothing came for
        // 30 s (a reconnect of the station loses it)
        if (sync >= 0 && (!joined || now - sync_seen_us > 30000000) && now - join_tried_us > 5000000) {
            join_tried_us = now;
            if (joined) {
                membership(sync, false);
            }
            joined = membership(sync, true);
            sync_seen_us = now;
            if (joined) {
                ESP_LOGI(kTag, "WLED audio sync on 239.0.0.1:%u", kWledSyncPort);
            }
        }
        if (warls < 0 && sync < 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        fd_set read_set;
        FD_ZERO(&read_set);
        int highest = -1;
        for (int fd : {warls, sync}) {
            if (fd >= 0) {
                FD_SET(fd, &read_set);
                highest = fd > highest ? fd : highest;
            }
        }
        timeval timeout{.tv_sec = 1, .tv_usec = 0};
        if (select(highest + 1, &read_set, nullptr, nullptr, &timeout) <= 0) {
            continue;
        }
        Received received{};
        if (warls >= 0 && FD_ISSET(warls, &read_set)) {
            const int size = recv(warls, buffer, sizeof(buffer), 0);
            if (size > 0 && parse_warls(buffer, static_cast<size_t>(size), received)) {
                take(received, Source::Warls, beats);
            }
        }
        if (sync >= 0 && FD_ISSET(sync, &read_set)) {
            const int size = recv(sync, buffer, sizeof(buffer), 0);
            if (size > 0 && parse_wled_sync(buffer, static_cast<size_t>(size), received)) {
                sync_seen_us = esp_timer_get_time();
                take(received, Source::WledSync, beats);
            }
        }
    }
}

} // namespace

esp_err_t load_udp()
{
    g_lock = xSemaphoreCreateMutex();
    if (g_lock == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    load_inputs();
    return ESP_OK;
}

esp_err_t start_udp_task()
{
    static bool started = false;
    if (started || g_lock == nullptr) {
        return started ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    started = true;
    // core 0 with Wi-Fi; the render task has core 1
    const BaseType_t core = portNUM_PROCESSORS > 1 ? 0 : tskNO_AFFINITY;
    if (xTaskCreatePinnedToCore(task, "audio_udp", 3072, nullptr, 4, nullptr, core) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void set_udp_inputs(const Inputs &inputs)
{
    Inputs stored = inputs;
    stored.version = Inputs{}.version;
    platform::Settings nvs(kNamespace, platform::Settings::Mode::ReadWrite);
    if (!nvs.ok() || nvs.set_blob(kKeyInputs, &stored, sizeof(stored)) != ESP_OK || nvs.commit() != ESP_OK) {
        ESP_LOGW(kTag, "the inputs could not be stored");
    }
    if (g_lock == nullptr) {
        g_inputs = stored;
        return;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_inputs = stored;
    g_changes++;
    xSemaphoreGive(g_lock);
}

Inputs udp_inputs()
{
    if (g_lock == nullptr) {
        return g_inputs;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    const Inputs out = g_inputs;
    xSemaphoreGive(g_lock);
    return out;
}

bool udp_frame(effects::Audio &out, Source &source, bool take_beat)
{
    if (g_lock == nullptr) {
        return false;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    const bool holds = g_source != Source::None && esp_timer_get_time() < g_until_us;
    if (holds) {
        out = g_audio;
        source = g_source;
        // the render task takes a pending beat once; the status shows the last packet's
        out.beat = take_beat ? g_beat_pending : g_audio.beat;
        if (take_beat) {
            g_beat_pending = false;
        }
    }
    xSemaphoreGive(g_lock);
    return holds;
}

uint32_t udp_packets()
{
    if (g_lock == nullptr) {
        return 0;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    const uint32_t out = g_packets;
    xSemaphoreGive(g_lock);
    return out;
}

} // namespace px::audio::detail
