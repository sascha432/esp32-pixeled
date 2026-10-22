// Presets (px/presets/presets.hpp, docs/leds.md section 10).

#include "px/presets/presets.hpp"

#include <cstdio>
#include <cstring>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "px/platform/storage.hpp"

namespace px::presets {

namespace {

constexpr const char *kTag = "presets";
constexpr const char *kPath = "/fs/presets.bin";
constexpr const char *kTempPath = "/fs/presets.tmp";

// The file: this header, then `count` presets as they are in memory. Another magic or version is
// not read (no presets).
struct Header {
    uint32_t magic;
    uint8_t version;
    uint8_t count;
    uint16_t preset_size;
};
constexpr uint32_t kMagic = 0x52505850; // "PXPR" in the file
constexpr uint8_t kVersion = 1;

SemaphoreHandle_t g_lock = nullptr;
Preset g_presets[kMaxPresets] = {};
size_t g_count = 0;
// the one applied last, for apply_next
size_t g_last = kMaxPresets;
uint32_t g_revision = 0;

// the file from the list; under the lock. Written next to it and renamed: a power cut keeps the old
// file or the new one (LittleFS renames atomically).
bool write_file(char *error, size_t error_size)
{
    if (platform::storage_status().fs != platform::FsState::Mounted) {
        std::snprintf(error, error_size, "the file system is not mounted");
        return false;
    }
    FILE *file = std::fopen(kTempPath, "wb");
    if (file == nullptr) {
        std::snprintf(error, error_size, "the presets could not be written");
        return false;
    }
    const Header header{kMagic, kVersion, static_cast<uint8_t>(g_count), sizeof(Preset)};
    bool ok = std::fwrite(&header, sizeof(header), 1, file) == 1;
    ok = ok && (g_count == 0 || std::fwrite(g_presets, sizeof(Preset), g_count, file) == g_count);
    ok = std::fclose(file) == 0 && ok;
    if (ok) {
        std::remove(kPath);
        ok = std::rename(kTempPath, kPath) == 0;
    }
    if (!ok) {
        std::remove(kTempPath);
        std::snprintf(error, error_size, "the presets could not be written");
        return false;
    }
    g_revision++;
    return true;
}

// a preset of the file or the backup: a name, an animation, a palette that are there
bool valid(const Preset &p)
{
    return p.name[0] != 0 && std::memchr(p.name, 0, kNameSize) != nullptr && p.effect < effects::count() &&
           p.effect < leds::kMaxEffects && p.style.palette < effects::palette_count() && p.style.rotation < 4;
}

bool same(const Preset &p, const leds::State &state, const leds::Style &style)
{
    return p.effect == state.effect && p.color.r == state.color.r && p.color.g == state.color.g &&
           p.color.b == state.color.b && p.speed == state.speed && p.brightness == state.brightness &&
           p.style.rotation == style.rotation && p.style.reactive == style.reactive &&
           p.style.strength == style.strength && p.style.palette == style.palette &&
           p.style.attack_ms == style.attack_ms && p.style.release_ms == style.release_ms;
}

} // namespace

esp_err_t start()
{
    ESP_RETURN_ON_FALSE(g_lock == nullptr, ESP_ERR_INVALID_STATE, kTag, "already started");
    g_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(g_lock != nullptr, ESP_ERR_NO_MEM, kTag, "lock");
    FILE *file = std::fopen(kPath, "rb");
    if (file == nullptr) {
        return ESP_OK; // none saved yet
    }
    Header header{};
    if (std::fread(&header, sizeof(header), 1, file) == 1 && header.magic == kMagic && header.version == kVersion &&
        header.preset_size == sizeof(Preset) && header.count <= kMaxPresets &&
        std::fread(g_presets, sizeof(Preset), header.count, file) == header.count) {
        // one that does not fit this firmware (an animation that is gone) is left out
        for (size_t i = 0; i < header.count; i++) {
            if (valid(g_presets[i])) {
                g_presets[g_count++] = g_presets[i];
            }
        }
        ESP_LOGI(kTag, "%u presets", static_cast<unsigned>(g_count));
    } else {
        ESP_LOGW(kTag, "%s is not a presets file of this firmware", kPath);
    }
    std::fclose(file);
    return ESP_OK;
}

size_t count()
{
    if (g_lock == nullptr) {
        return 0;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    const size_t out = g_count;
    xSemaphoreGive(g_lock);
    return out;
}

bool get(size_t index, Preset &out)
{
    if (g_lock == nullptr) {
        return false;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    const bool ok = index < g_count;
    if (ok) {
        out = g_presets[index];
    }
    xSemaphoreGive(g_lock);
    return ok;
}

size_t find(const char *name)
{
    if (g_lock == nullptr) {
        return 0;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    size_t i = 0;
    while (i < g_count && std::strcmp(g_presets[i].name, name) != 0) {
        i++;
    }
    xSemaphoreGive(g_lock);
    return i;
}

bool save_current(const char *name, char *error, size_t error_size)
{
    if (g_lock == nullptr) {
        std::snprintf(error, error_size, "the presets do not run");
        return false;
    }
    if (name == nullptr || name[0] == 0 || std::strlen(name) >= kNameSize) {
        std::snprintf(error, error_size, "name: 1 to %u bytes", static_cast<unsigned>(kNameSize - 1));
        return false;
    }
    const leds::Status s = leds::status();
    Preset p{};
    std::snprintf(p.name, sizeof(p.name), "%s", name);
    p.effect = s.state.effect;
    p.color = s.state.color;
    p.speed = s.state.speed;
    p.brightness = s.state.brightness;
    p.style = s.style;
    xSemaphoreTake(g_lock, portMAX_DELAY);
    size_t i = 0;
    while (i < g_count && std::strcmp(g_presets[i].name, name) != 0) {
        i++;
    }
    bool ok = i < kMaxPresets;
    if (!ok) {
        std::snprintf(error, error_size, "at most %u presets", static_cast<unsigned>(kMaxPresets));
    } else {
        const Preset before = g_presets[i];
        const size_t count_before = g_count;
        g_presets[i] = p;
        g_count = i == g_count ? g_count + 1 : g_count;
        ok = write_file(error, error_size);
        if (!ok) {
            g_presets[i] = before;
            g_count = count_before;
        } else {
            g_last = i;
        }
    }
    xSemaphoreGive(g_lock);
    if (ok) {
        ESP_LOGI(kTag, "saved '%s'", name);
    }
    return ok;
}

bool remove(size_t index, char *error, size_t error_size)
{
    if (g_lock == nullptr) {
        std::snprintf(error, error_size, "the presets do not run");
        return false;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    bool ok = index < g_count;
    if (!ok) {
        std::snprintf(error, error_size, "no such preset");
    } else {
        Preset before[kMaxPresets];
        std::memcpy(before, g_presets, sizeof(before));
        std::memmove(&g_presets[index], &g_presets[index + 1], (g_count - index - 1) * sizeof(Preset));
        g_count--;
        ok = write_file(error, error_size);
        if (!ok) {
            std::memcpy(g_presets, before, sizeof(before));
            g_count++;
        }
        g_last = kMaxPresets;
    }
    xSemaphoreGive(g_lock);
    return ok;
}

bool set_all(const Preset *presets, size_t count, char *error, size_t error_size)
{
    if (g_lock == nullptr) {
        std::snprintf(error, error_size, "the presets do not run");
        return false;
    }
    if (count > kMaxPresets) {
        std::snprintf(error, error_size, "at most %u presets", static_cast<unsigned>(kMaxPresets));
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        if (!valid(presets[i])) {
            std::snprintf(error, error_size, "preset %u is not one of this firmware", static_cast<unsigned>(i + 1));
            return false;
        }
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    Preset before[kMaxPresets];
    std::memcpy(before, g_presets, sizeof(before));
    const size_t count_before = g_count;
    std::memcpy(g_presets, presets, count * sizeof(Preset));
    g_count = count;
    const bool ok = write_file(error, error_size);
    if (!ok) {
        std::memcpy(g_presets, before, sizeof(before));
        g_count = count_before;
    }
    g_last = kMaxPresets;
    xSemaphoreGive(g_lock);
    return ok;
}

bool apply(size_t index)
{
    Preset p{};
    if (!get(index, p)) {
        return false;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_last = index;
    xSemaphoreGive(g_lock);
    ESP_LOGI(kTag, "'%s'", p.name);
    // the style first: the animation starts with it
    leds::set_style(p.effect, p.style);
    leds::set_effect(p.effect);
    leds::set_color(p.color);
    leds::set_speed(p.speed);
    leds::set_brightness(p.brightness);
    leds::set_on(true);
    return true;
}

bool apply_next()
{
    const size_t n = count();
    if (n == 0) {
        return false;
    }
    size_t from = current();
    if (from >= n) {
        xSemaphoreTake(g_lock, portMAX_DELAY);
        from = g_last;
        xSemaphoreGive(g_lock);
    }
    return apply(from < n ? (from + 1) % n : 0);
}

size_t current()
{
    if (g_lock == nullptr) {
        return 0;
    }
    const leds::Status s = leds::status();
    xSemaphoreTake(g_lock, portMAX_DELAY);
    size_t i = 0;
    while (i < g_count && !same(g_presets[i], s.state, s.style)) {
        i++;
    }
    // of several that match, the one applied last
    if (g_last < g_count && i < g_count && same(g_presets[g_last], s.state, s.style)) {
        i = g_last;
    }
    xSemaphoreGive(g_lock);
    return i;
}

uint32_t revision()
{
    if (g_lock == nullptr) {
        return 0;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    const uint32_t out = g_revision;
    xSemaphoreGive(g_lock);
    return out;
}

} // namespace px::presets
