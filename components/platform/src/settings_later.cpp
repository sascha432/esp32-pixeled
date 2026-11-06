// Delayed settings writes (px/platform/settings.hpp). The values wait in a small table, their
// bytes one after the other in a buffer they share (the LED styles take half of it, the state a
// few bytes); an esp_timer, restarted by every call but not past kWriteLaterMaxMs after the first
// pending value, writes them from the esp_timer task.

#include <algorithm>
#include <cstring>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "px/platform/settings.hpp"

namespace px::platform {

namespace {

constexpr const char *kTag = "settings";
constexpr size_t kNameSize = 16; // NVS namespace and key names have up to 15 characters

static_assert(kMaxPendingSize <= kPendingBytes);

struct Pending {
    char name_space[kNameSize];
    char key[kNameSize];
    // where its value is in g_data
    uint16_t offset;
    uint16_t size;
};

// g_lock is held while the values are written, so a discard waits for a write in progress. In
// PSRAM on the S3 (docs/architecture.md section 11): the tasks that set values and the timer
// task; NVS copies a value from PSRAM through a buffer of its own.
EXT_RAM_BSS_ATTR Pending g_pending[kMaxPendingWrites];
EXT_RAM_BSS_ATTR uint8_t g_data[kPendingBytes];
size_t g_count = 0;
size_t g_used = 0; // bytes of g_data
int64_t g_first_us = 0; // when the oldest pending value came
SemaphoreHandle_t g_lock = nullptr;
esp_timer_handle_t g_timer = nullptr;

// with g_lock held
void write_locked()
{
    for (size_t i = 0; i < g_count; i++) {
        Settings settings(g_pending[i].name_space, Settings::Mode::ReadWrite);
        esp_err_t err = settings.set_blob(g_pending[i].key, g_data + g_pending[i].offset, g_pending[i].size);
        if (err == ESP_OK) {
            err = settings.commit();
        }
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "%s/%s not stored: %s", g_pending[i].name_space, g_pending[i].key, esp_err_to_name(err));
        }
    }
    g_count = 0;
    g_used = 0;
}

void write_all()
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    write_locked();
    xSemaphoreGive(g_lock);
}

esp_err_t init()
{
    g_lock = xSemaphoreCreateMutex();
    if (g_lock == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    esp_timer_create_args_t args{};
    args.callback = [](void *) { write_all(); };
    args.name = "settings_later";
    esp_err_t err = esp_timer_create(&args, &g_timer);
    if (err == ESP_OK) {
        err = esp_register_shutdown_handler(write_pending_settings);
    }
    return err;
}

} // namespace

esp_err_t set_blob_later(const char *name_space, const char *key, const void *data, size_t size)
{
    if (size > kMaxPendingSize || std::strlen(name_space) >= kNameSize || std::strlen(key) >= kNameSize) {
        return ESP_ERR_INVALID_ARG;
    }
    // created on the first call, from whichever task makes it
    static const esp_err_t init_result = init();
    if (init_result != ESP_OK) {
        return init_result;
    }

    xSemaphoreTake(g_lock, portMAX_DELAY);
    Pending *slot = nullptr;
    for (size_t i = 0; i < g_count; i++) {
        if (std::strcmp(g_pending[i].name_space, name_space) == 0 && std::strcmp(g_pending[i].key, key) == 0) {
            slot = &g_pending[i];
        }
    }
    if (slot != nullptr && slot->size != size) {
        // a key's value has one size; one that changed is written with the others first
        write_locked();
        slot = nullptr;
    }
    if (slot == nullptr) {
        if (g_count == kMaxPendingWrites || g_used + size > kPendingBytes) {
            // a full table is written now rather than a value lost
            write_locked();
        }
        if (g_count == 0) {
            g_first_us = esp_timer_get_time();
        }
        slot = &g_pending[g_count++];
        std::strcpy(slot->name_space, name_space);
        std::strcpy(slot->key, key);
        slot->offset = static_cast<uint16_t>(g_used);
        slot->size = static_cast<uint16_t>(size);
        g_used += size;
    }
    std::memcpy(g_data + slot->offset, data, size);
    // kWriteLaterMs after this call, but not later than kWriteLaterMaxMs after the first one
    const int64_t until_max = g_first_us + static_cast<int64_t>(kWriteLaterMaxMs) * 1000 - esp_timer_get_time();
    const int64_t delay = std::clamp<int64_t>(until_max, 1, static_cast<int64_t>(kWriteLaterMs) * 1000);
    xSemaphoreGive(g_lock);

    esp_timer_stop(g_timer); // not running is fine
    return esp_timer_start_once(g_timer, static_cast<uint64_t>(delay));
}

bool pending_blob(const char *name_space, const char *key, void *out, size_t size)
{
    if (g_lock == nullptr) {
        return false;
    }
    bool found = false;
    xSemaphoreTake(g_lock, portMAX_DELAY);
    for (size_t i = 0; i < g_count && !found; i++) {
        const Pending &p = g_pending[i];
        if (p.size == size && std::strcmp(p.name_space, name_space) == 0 && std::strcmp(p.key, key) == 0) {
            std::memcpy(out, g_data + p.offset, size);
            found = true;
        }
    }
    xSemaphoreGive(g_lock);
    return found;
}

void write_pending_settings()
{
    if (g_lock == nullptr) {
        return;
    }
    esp_timer_stop(g_timer);
    write_all();
}

void discard_pending_settings()
{
    if (g_lock == nullptr) {
        return;
    }
    esp_timer_stop(g_timer);
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_count = 0;
    g_used = 0;
    xSemaphoreGive(g_lock);
}

} // namespace px::platform
