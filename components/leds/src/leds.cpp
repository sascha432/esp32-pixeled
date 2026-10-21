// The render task (px/leds/leds.hpp, docs/leds.md section 3).

#include "px/leds/leds.hpp"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <iterator>
#include <utility>

#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "px/effects/overlay.hpp"
#include "px/memory/large.hpp"
#include "px/output/output.hpp"
#include "px/platform/settings.hpp"

#include "frame.hpp"

namespace px::leds {

namespace {

constexpr const char *kTag = "leds";
constexpr const char *kNamespace = "leds";
constexpr const char *kKeyLayout = "layout";
constexpr const char *kKeyConfig = "config";
constexpr const char *kKeyState = "state";
constexpr const char *kKeyLanes = "lanes";
constexpr const char *kKeyMotion = "motion";
constexpr const char *kKeyStyles = "styles";
constexpr const char *kKeyOverlay = "overlay";
constexpr const char *kKeyBoot = "boot";

// what changes while running and is kept over a restart for Config::power_on
struct Boot {
    uint8_t version = 1;
    // motion switched the LEDs off: the next motion switches them on
    bool off_by_motion = false;
};

// the previous versions of the stored Config and Styles: read once, their values kept
struct ConfigV2 {
    uint8_t version;
    uint32_t power_limit_mw;
    uint16_t channel_mw;
    uint16_t idle_mw;
    uint8_t fps;
    uint16_t fade_ms;
    uint16_t settle_ms;
    output::Driver driver;
    bool dither;
};

struct StylesV2 {
    uint8_t version;
    struct {
        uint8_t rotation;
        bool reactive;
        uint8_t strength;
    } style[16];
};

enum class MsgType : uint8_t {
    On,
    Brightness,
    Effect,
    Color,
    Speed,
    Layout,
    Config,
    Lanes,
    AutoOff,
    Motion,
    Suspend,
    Style,
    Overlay,
};

struct Msg {
    MsgType type;
    uint32_t value;
};

// everything the task owns; the layout and config of a message wait in g_pending under the lock
struct Task {
    Start start;
    Layout layout;
    Config config;
    State state;
    Lanes lanes;
    AutoOff auto_off;
    Styles styles;
    Overlay overlay;
    bool motion = false;
    // motion switched the LEDs off: the next motion switches them on
    bool off_by_motion = false;
    // what each lane does to its colors, from `lanes` and Config::dither
    detail::LaneMix mix[output::kMaxLanes];
    layout::Table table;
    effects::Rgb *canvas = nullptr;
    // the animation's memory (cleared for another animation, rotation or canvas) and the spare
    // canvas of the quarter turns (allocated when one is used)
    uint8_t *memory = nullptr;
    size_t memory_size = 0;
    uint32_t memory_for = 0xffffffff;
    effects::Rgb *spare = nullptr;
    // the crossfade: the previous animation on its own canvas with its own memory (allocated with
    // the first switch), until fade_until_us
    effects::Rgb *fade_canvas = nullptr;
    uint8_t *fade_memory = nullptr;
    uint8_t fade_effect = 0;
    Style fade_style;
    int64_t fade_start_us = 0;
    bool fading = false;
    bool hears_audio = false;
    uint8_t *frame = nullptr;
    size_t frame_size = 0;
    bool lanes_ready = false;
    bool suspended = false;
    // the fade: the level in 1/256 steps
    uint32_t level = 0;
    bool supply_on = false;
    int64_t settle_until_us = 0;
    // a dark frame is out since the level reached 0
    bool dark_sent = true;
    int64_t last_frame_us = 0;
    uint32_t frames = 0;
    // every frame sent, for the dithering
    uint32_t frame_number = 0;
    int64_t fps_since_us = 0;
    uint16_t fps = 0;
    uint32_t power_mw = 0;
    bool limited = false;
    uint32_t render_us = 0;
    uint32_t compose_us = 0;
    uint32_t send_us = 0;
    char error[96] = {};
};

Task t;
detail::Gamma *g_gamma = nullptr;

// Crashes in a row, kept over a restart: a stored setting that crashes the LEDs (a driver, a
// layout) would otherwise restart the device forever. After kMaxCrashes the LEDs start off with
// RMT; kStableUs of running clear the count.
constexpr uint32_t kCrashMagic = 0x50584c43; // "PXLC"
constexpr uint32_t kMaxCrashes = 3;
constexpr int64_t kStableUs = 30 * 1000000;
RTC_NOINIT_ATTR uint32_t g_crash_magic;
RTC_NOINIT_ATTR uint32_t g_crashes;
QueueHandle_t g_queue = nullptr;
SemaphoreHandle_t g_lock = nullptr;
Status g_status = {};
Layout g_layout = {};
Config g_config = {};
Layout g_pending_layout = {};
Config g_pending_config = {};
Lanes g_lanes = {};
Lanes g_pending_lanes = {};
AutoOff g_auto_off = {};
Styles g_styles = {};
Styles g_pending_styles = {};
Overlay g_overlay = {};
Overlay g_pending_overlay = {};
// the preview (leds::preview): the canvas copied while someone asked within the last 2 s
uint8_t *g_preview = nullptr;
size_t g_preview_size = 0;
uint16_t g_preview_width = 0;
uint16_t g_preview_height = 0;
int64_t g_preview_asked_us = -10000000;

void update_mix()
{
    for (size_t i = 0; i < output::kMaxLanes; i++) {
        const LaneSettings &l = t.lanes.lane[i];
        const auto factor = [&](uint8_t balance) {
            return static_cast<uint16_t>((static_cast<uint32_t>(l.level) + 1) * (balance + 1u) >> 8);
        };
        t.mix[i] = {.red = factor(l.red), .green = factor(l.green), .blue = factor(l.blue),
                    .dither = t.config.dither && l.dither};
    }
}

void post(MsgType type, uint32_t value = 0)
{
    if (g_queue == nullptr) {
        return;
    }
    const Msg msg{type, value};
    if (xQueueSend(g_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(kTag, "command dropped, the queue is full");
    }
}

// ------------------------------------------------------------------------------------ storage

template <class T>
void load(const char *key, T &out)
{
    platform::Settings settings(kNamespace, platform::Settings::Mode::Read);
    T stored{};
    size_t size = sizeof(stored);
    if (settings.ok() && settings.get_blob(key, &stored, size) == ESP_OK && size == sizeof(stored) &&
        stored.version == T{}.version) {
        out = stored;
    }
}

template <class T>
void save(const char *key, const T &value)
{
    platform::Settings settings(kNamespace, platform::Settings::Mode::ReadWrite);
    if (!settings.ok() || settings.set_blob(key, &value, sizeof(value)) != ESP_OK || settings.commit() != ESP_OK) {
        ESP_LOGW(kTag, "the %s could not be stored", key);
    }
}

// for values set by sliders and Home Assistant numbers (the write policy, px/platform/settings.hpp)
template <class T>
void save_later(const char *key, const T &value)
{
    static_assert(sizeof(T) <= platform::kMaxPendingSize);
    if (platform::set_blob_later(kNamespace, key, &value, sizeof(value)) != ESP_OK) {
        ESP_LOGW(kTag, "the %s could not be stored", key);
    }
}

void save_state_later()
{
    save_later(kKeyState, t.state);
}

// the stored blob of `key` in the layout of the previous version `Old`; false when it is not one
template <class Old>
bool load_old(const char *key, uint8_t version, Old &out)
{
    platform::Settings settings(kNamespace, platform::Settings::Mode::Read);
    size_t size = sizeof(out);
    return settings.ok() && settings.get_blob(key, &out, size) == ESP_OK && size == sizeof(out) &&
           out.version == version;
}

// the config and the styles of the version before: their values kept, the new fields at default
void load_config_and_styles()
{
    load(kKeyConfig, t.config);
    if (ConfigV2 old{}; load_old(kKeyConfig, 2, old)) {
        t.config.power_limit_mw = old.power_limit_mw;
        t.config.channel_mw = old.channel_mw;
        t.config.idle_mw = old.idle_mw;
        t.config.fps = old.fps;
        t.config.fade_ms = old.fade_ms;
        t.config.settle_ms = old.settle_ms;
        t.config.driver = old.driver;
        t.config.dither = old.dither;
        save(kKeyConfig, t.config);
        ESP_LOGI(kTag, "the config of version 2 taken over");
    }
    load(kKeyStyles, t.styles);
    if (StylesV2 old{}; load_old(kKeyStyles, 2, old)) {
        for (size_t i = 0; i < std::size(old.style); i++) {
            t.styles.style[i].rotation = old.style[i].rotation & 3;
            t.styles.style[i].reactive = old.style[i].reactive;
            t.styles.style[i].strength = old.style[i].strength;
        }
        save(kKeyStyles, t.styles);
        ESP_LOGI(kTag, "the styles of version 2 taken over");
    }
    for (Style &style : t.styles.style) {
        style.attack_ms = style.attack_ms > effects::kMaxAttackMs ? Style{}.attack_ms : style.attack_ms;
        style.release_ms = style.release_ms > effects::kMaxReleaseMs ? Style{}.release_ms : style.release_ms;
    }
}

void save_boot()
{
    const Boot boot{.version = Boot{}.version, .off_by_motion = t.off_by_motion};
    save(kKeyBoot, boot);
}

// motion switched the LEDs off, or not any more; stored for Config::power_on
void set_off_by_motion(bool off)
{
    if (t.off_by_motion != off) {
        t.off_by_motion = off;
        save_boot();
    }
}

// ------------------------------------------------------------------------------------ layout

void set_error(const char *text)
{
    std::snprintf(t.error, sizeof(t.error), "%s", text);
    if (text[0] != 0) {
        ESP_LOGW(kTag, "%s", text);
    }
}

// the table, the buffers and the lanes of t.layout; on a problem nothing is shown
void apply_layout()
{
    output::stop();
    t.lanes_ready = false;
    memory::release_large(t.canvas);
    memory::release_large(t.frame);
    memory::release_large(t.memory);
    memory::release_large(t.spare);
    memory::release_large(t.fade_canvas);
    memory::release_large(t.fade_memory);
    t.canvas = nullptr;
    t.frame = nullptr;
    t.memory = nullptr;
    t.spare = nullptr;
    t.fade_canvas = nullptr;
    t.fade_memory = nullptr;
    t.fading = false;
    t.memory_for = 0xffffffff;
    t.table = {};
    if (t.layout.width == 0 || t.layout.panel_count == 0) {
        set_error("no layout");
        return;
    }
    auto table = layout::build(t.layout.width, t.layout.height, {t.layout.panels, t.layout.panel_count});
    if (!table) {
        set_error(table.error().text);
        return;
    }
    t.table = std::move(*table);
    output::Lane lanes[output::kMaxLanes] = {};
    for (size_t i = 0; i < t.table.lanes; i++) {
        if (i >= t.start.lane_count || t.start.lane_gpios[i] < 0) {
            char text[48];
            std::snprintf(text, sizeof(text), "lane %u has no pin on this board", static_cast<unsigned>(i + 1));
            set_error(text);
            return;
        }
        lanes[i] = {.gpio = t.start.lane_gpios[i], .pixels = t.table.lane_pixels[i], .order = t.lanes.lane[i].order};
    }
    const size_t pixels = static_cast<size_t>(t.layout.width) * t.layout.height;
    t.frame_size = t.table.leds * 3;
    t.canvas = static_cast<effects::Rgb *>(memory::allocate_large(pixels * sizeof(effects::Rgb)));
    t.frame = static_cast<uint8_t *>(memory::allocate_large(t.frame_size));
    t.memory_size = effects::memory_size(t.layout.width, t.layout.height);
    t.memory = static_cast<uint8_t *>(memory::allocate_large(t.memory_size));
    if (t.canvas == nullptr || t.frame == nullptr || t.memory == nullptr) {
        set_error("no memory for the frame");
        return;
    }
    std::memset(t.frame, 0, t.frame_size);
    if (output::start(lanes, t.table.lanes, t.config.driver) != ESP_OK) {
        set_error("the outputs did not start");
        return;
    }
    t.lanes_ready = true;
    t.dark_sent = false;
    set_error("");
    ESP_LOGI(kTag, "canvas %ux%u, %u LEDs on %u lanes", t.layout.width, t.layout.height,
             static_cast<unsigned>(t.table.leds), static_cast<unsigned>(t.table.lanes));
}

// ------------------------------------------------------------------------------------ frames

void supply(bool on)
{
    if (t.supply_on == on) {
        return;
    }
    t.supply_on = on;
    if (t.start.supply != nullptr) {
        t.start.supply(on);
    }
    if (on) {
        t.settle_until_us = esp_timer_get_time() + static_cast<int64_t>(t.config.settle_ms) * 1000;
    }
    ESP_LOGI(kTag, "supply %s", on ? "on" : "off");
}

void send(int64_t now_us)
{
    const detail::PowerModel model{t.config.channel_mw, t.config.idle_mw};
    t.power_mw = detail::limit_power(t.frame, t.table.leds, model, t.config.power_limit_mw, t.limited);
    const int64_t before = esp_timer_get_time();
    const esp_err_t err = output::show(t.frame, t.frame_size);
    t.send_us = static_cast<uint32_t>(esp_timer_get_time() - before);
    if (err != ESP_OK) {
        return;
    }
    t.frames++;
    if (now_us - t.fps_since_us >= 1000000) {
        t.fps = static_cast<uint16_t>(t.frames * 1000000LL / (now_us - t.fps_since_us));
        t.frames = 0;
        t.fps_since_us = now_us;
    }
}

bool has_audio_source()
{
    return t.start.audio != nullptr && (t.start.has_audio == nullptr || t.start.has_audio());
}

// animation `effect` with its style on `pixels` (the canvas' size) with its `memory`
void draw(uint8_t effect, const Style &style, effects::Rgb *pixels, uint8_t *memory, const effects::Audio *audio,
          uint32_t ms)
{
    // the spare canvas once a quarter turn needs it; without the memory for it, not turned
    if ((style.rotation & 1) != 0 && t.spare == nullptr) {
        t.spare = static_cast<effects::Rgb *>(
            memory::allocate_large(static_cast<size_t>(t.layout.width) * t.layout.height * sizeof(effects::Rgb)));
    }
    effects::Canvas canvas{t.layout.width, t.layout.height, pixels, memory, t.memory_size, t.spare};
    const effects::Params params{.color = t.state.color,
                                 .speed = t.state.speed,
                                 .audio = audio,
                                 .rotation = style.rotation,
                                 .reactive = style.reactive,
                                 .strength = style.strength,
                                 .attack_ms = style.attack_ms,
                                 .release_ms = style.release_ms};
    effects::render(effect, canvas, params, ms);
}

// Starts the crossfade from the animation shown now: it keeps its memory (swapped to the fade's),
// the new one starts with clear memory. Without the memory for a second canvas: no crossfade.
void start_crossfade(uint8_t from)
{
    if (t.config.crossfade_ms == 0 || !t.lanes_ready || t.canvas == nullptr || t.level == 0) {
        t.fading = false;
        return;
    }
    const size_t pixels = static_cast<size_t>(t.layout.width) * t.layout.height;
    if (t.fade_canvas == nullptr) {
        t.fade_canvas = static_cast<effects::Rgb *>(memory::allocate_large(pixels * sizeof(effects::Rgb)));
    }
    if (t.fade_memory == nullptr) {
        t.fade_memory = static_cast<uint8_t *>(memory::allocate_large(t.memory_size));
    }
    if (t.fade_canvas == nullptr || t.fade_memory == nullptr) {
        ESP_LOGW(kTag, "no memory for the crossfade");
        t.fading = false;
        return;
    }
    std::swap(t.memory, t.fade_memory);
    // the new animation's memory is cleared with the next frame
    t.memory_for = 0xffffffff;
    t.fade_effect = from;
    t.fade_style = from < kMaxEffects ? t.styles.style[from] : Style{};
    t.fade_start_us = esp_timer_get_time();
    t.fading = true;
}

// the text or the clock over the canvas
void draw_overlay(uint32_t ms)
{
    const Overlay &o = t.overlay;
    if (o.mode == OverlayMode::Off) {
        return;
    }
    effects::Canvas canvas{t.layout.width, t.layout.height, t.canvas};
    if (o.mode == OverlayMode::Text) {
        const uint16_t width = effects::text_width(o.text);
        const int y = (static_cast<int>(t.layout.height) - effects::kTextHeight) / 2;
        effects::draw_text(canvas, o.text, effects::text_x(t.layout.width, width, o.speed, ms), y, o.color);
        return;
    }
    // the clock: only once the time is known (SNTP)
    const time_t now = std::time(nullptr);
    if (now < 1700000000) {
        return;
    }
    tm local{};
    localtime_r(&now, &local);
    const int hours = o.clock_24h ? local.tm_hour : (local.tm_hour + 11) % 12 + 1;
    effects::draw_clock(canvas, static_cast<uint8_t>(hours), static_cast<uint8_t>(local.tm_min), local.tm_sec % 2 == 0,
                        o.color);
}

// the canvas for leds::preview, while someone asks
void copy_preview()
{
    const size_t size = static_cast<size_t>(t.layout.width) * t.layout.height * 3;
    xSemaphoreTake(g_lock, portMAX_DELAY);
    const bool wanted = esp_timer_get_time() - g_preview_asked_us < 2000000;
    if (wanted && g_preview_size != size) {
        memory::release_large(g_preview);
        g_preview = static_cast<uint8_t *>(memory::allocate_large(size));
        g_preview_size = g_preview != nullptr ? size : 0;
    } else if (!wanted && g_preview != nullptr) {
        memory::release_large(g_preview);
        g_preview = nullptr;
        g_preview_size = 0;
    }
    if (wanted && g_preview != nullptr) {
        std::memcpy(g_preview, t.canvas, size);
        g_preview_width = t.layout.width;
        g_preview_height = t.layout.height;
    }
    xSemaphoreGive(g_lock);
}

void render_frame(int64_t now_us)
{
    if (!t.lanes_ready || t.suspended) {
        return;
    }
    // the fade toward the brightness, or to dark when off
    const uint32_t target = t.state.on ? t.state.brightness * 256u : 0;
    const auto elapsed_ms = static_cast<uint32_t>((now_us - t.last_frame_us) / 1000);
    t.last_frame_us = now_us;
    const uint32_t step = t.config.fade_ms == 0 ? 65535 : 255u * 256u * elapsed_ms / t.config.fade_ms + 1;
    if (t.level < target) {
        t.level = t.level + step < target ? t.level + step : target;
    } else if (t.level > target) {
        t.level = t.level > target + step ? t.level - step : target;
    }

    if (t.level == 0 && target == 0) {
        // dark: one dark frame, then the supply goes off
        if (!t.dark_sent && t.supply_on) {
            std::memset(t.frame, 0, t.frame_size);
            send(now_us);
        }
        t.dark_sent = true;
        supply(false);
        t.fps = 0;
        return;
    }
    if (!t.supply_on) {
        supply(true);
    }
    if (now_us < t.settle_until_us) {
        return;
    }
    t.dark_sent = false;
    const Style style = t.state.effect < kMaxEffects ? t.styles.style[t.state.effect] : Style{};
    // another animation or rotation starts with clear memory
    const uint32_t memory_for = (static_cast<uint32_t>(t.state.effect) << 8) | style.rotation;
    if (memory_for != t.memory_for) {
        std::memset(t.memory, 0, t.memory_size);
        t.memory_for = memory_for;
    }
    effects::Audio audio{};
    const bool has_audio = has_audio_source();
    if (has_audio) {
        t.start.audio(audio);
    }
    t.hears_audio = audio.active;
    const auto ms = static_cast<uint32_t>(now_us / 1000);
    const int64_t begin = esp_timer_get_time();
    draw(t.state.effect, style, t.canvas, t.memory, has_audio ? &audio : nullptr, ms);
    // the crossfade: the previous animation, less of it with every frame
    if (t.fading) {
        const int64_t into = now_us - t.fade_start_us;
        const int64_t length = static_cast<int64_t>(t.config.crossfade_ms) * 1000;
        if (into >= length || t.fade_canvas == nullptr) {
            t.fading = false;
        } else {
            draw(t.fade_effect, t.fade_style, t.fade_canvas, t.fade_memory, has_audio ? &audio : nullptr, ms);
            effects::blend(t.canvas, t.fade_canvas, static_cast<size_t>(t.layout.width) * t.layout.height,
                           static_cast<uint8_t>(255 - into * 255 / length));
        }
    }
    draw_overlay(ms);
    copy_preview();
    const int64_t rendered = esp_timer_get_time();
    detail::compose(t.canvas, t.table, static_cast<uint8_t>(t.level >> 8), *g_gamma, t.mix, t.frame_number++,
                    t.frame);
    t.render_us = static_cast<uint32_t>(rendered - begin);
    t.compose_us = static_cast<uint32_t>(esp_timer_get_time() - rendered);
    send(now_us);
}

// ------------------------------------------------------------------------------------ the task

// the sensor's hold is the auto off time
void apply_motion_hold()
{
    if (t.start.set_motion_hold != nullptr) {
        t.start.set_motion_hold(static_cast<uint32_t>(t.auto_off.minutes) * 60);
    }
}

void handle(const Msg &msg)
{
    switch (msg.type) {
    case MsgType::On:
        t.state.on = msg.value != 0;
        // a person switched: motion does not switch back on what they switched off
        set_off_by_motion(false);
        save_state_later();
        break;
    case MsgType::Brightness:
        t.state.brightness = static_cast<uint8_t>(msg.value);
        save_state_later();
        break;
    case MsgType::Effect:
        if (t.state.effect != msg.value) {
            start_crossfade(t.state.effect);
        }
        t.state.effect = static_cast<uint8_t>(msg.value);
        save_state_later();
        break;
    case MsgType::Color:
        t.state.color = {static_cast<uint8_t>(msg.value >> 16), static_cast<uint8_t>(msg.value >> 8),
                         static_cast<uint8_t>(msg.value)};
        save_state_later();
        break;
    case MsgType::Speed:
        t.state.speed = static_cast<uint8_t>(msg.value);
        save_state_later();
        break;
    case MsgType::Layout:
        xSemaphoreTake(g_lock, portMAX_DELAY);
        t.layout = g_pending_layout;
        xSemaphoreGive(g_lock);
        save(kKeyLayout, t.layout);
        apply_layout();
        break;
    case MsgType::Config:
        xSemaphoreTake(g_lock, portMAX_DELAY);
        t.config = g_pending_config;
        xSemaphoreGive(g_lock);
        save(kKeyConfig, t.config);
        update_mix();
        // another output: the lanes start again
        if (t.lanes_ready && output::driver() != t.config.driver) {
            apply_layout();
        }
        break;
    case MsgType::Lanes: {
        xSemaphoreTake(g_lock, portMAX_DELAY);
        const Lanes before = t.lanes;
        t.lanes = g_pending_lanes;
        xSemaphoreGive(g_lock);
        save(kKeyLanes, t.lanes);
        update_mix();
        // another color order: the output takes it when the lanes start again
        bool reorder = false;
        for (size_t i = 0; i < output::kMaxLanes; i++) {
            reorder |= before.lane[i].order != t.lanes.lane[i].order;
        }
        if (reorder && t.lanes_ready) {
            apply_layout();
        }
        break;
    }
    case MsgType::AutoOff:
        t.auto_off = {.version = AutoOff{}.version, .enabled = (msg.value & 0x10000) != 0,
                      .minutes = static_cast<uint16_t>(msg.value & 0xffff)};
        save(kKeyMotion, t.auto_off);
        apply_motion_hold();
        break;
    case MsgType::Motion:
        t.motion = msg.value != 0;
        if (t.motion) {
            if (t.off_by_motion) {
                ESP_LOGI(kTag, "motion: on again");
                set_off_by_motion(false);
                t.state.on = true;
                save_state_later();
            }
        } else if (t.auto_off.enabled && t.state.on) {
            // the sensor's hold ran out without another motion
            ESP_LOGI(kTag, "no motion for %u min: off", t.auto_off.minutes);
            t.state.on = false;
            set_off_by_motion(true);
            save_state_later();
        }
        break;
    case MsgType::Style:
        xSemaphoreTake(g_lock, portMAX_DELAY);
        t.styles = g_pending_styles;
        xSemaphoreGive(g_lock);
        // the strength, attack and release sliders send while they move
        save_later(kKeyStyles, t.styles);
        break;
    case MsgType::Overlay:
        xSemaphoreTake(g_lock, portMAX_DELAY);
        t.overlay = g_pending_overlay;
        xSemaphoreGive(g_lock);
        save(kKeyOverlay, t.overlay);
        break;
    case MsgType::Suspend:
        t.suspended = msg.value != 0;
        if (t.suspended) {
            // the console starts the lanes its own way; they are set up again on resume
            t.lanes_ready = false;
        } else {
            apply_layout();
        }
        break;
    }
}

void publish()
{
    const bool has_audio = has_audio_source();
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_layout = t.layout;
    g_config = t.config;
    g_lanes = t.lanes;
    g_auto_off = t.auto_off;
    g_styles = t.styles;
    g_overlay = t.overlay;
    g_status.style = t.state.effect < kMaxEffects ? t.styles.style[t.state.effect] : Style{};
    g_status.has_audio = has_audio;
    g_status.hears_audio = t.hears_audio && t.supply_on;
    g_status.has_motion = t.start.has_motion;
    g_status.motion = t.motion;
    g_status.off_by_motion = t.off_by_motion;
    g_status.auto_off_in_s = t.auto_off.enabled && t.state.on && t.motion && t.start.motion_hold_left != nullptr
                                 ? t.start.motion_hold_left()
                                 : 0;
    g_status.state = t.state;
    g_status.width = t.layout.width;
    g_status.height = t.layout.height;
    g_status.leds = static_cast<uint32_t>(t.table.leds);
    g_status.lanes = static_cast<uint8_t>(t.table.lanes);
    g_status.available_lanes = 0;
    for (size_t i = 0; i < t.start.lane_count && t.start.lane_gpios[i] >= 0; i++) {
        g_status.available_lanes++;
    }
    g_status.level = static_cast<uint8_t>(t.level >> 8);
    g_status.supply_on = t.supply_on;
    g_status.fps = t.fps;
    g_status.render_us = t.render_us;
    g_status.compose_us = t.compose_us;
    g_status.send_us = t.send_us;
    g_status.power_mw = t.power_mw;
    g_status.limited = t.limited;
    std::snprintf(g_status.error, sizeof(g_status.error), "%s", t.suspended ? "suspended by the console" : t.error);
    xSemaphoreGive(g_lock);
}

void task(void *)
{
    bool counted = g_crashes > 0;
    apply_layout();
    if (g_crashes >= kMaxCrashes) {
        set_error("the LEDs crashed the device three times: started off with RMT, check the settings");
    }
    publish();
    t.last_frame_us = esp_timer_get_time();
    int64_t next_us = t.last_frame_us;
    Msg msg;
    for (;;) {
        const int64_t now = esp_timer_get_time();
        // idle (nothing to show, dark and the supply off): wait for a command only
        const bool idle = !t.lanes_ready || t.suspended || (!t.state.on && t.level == 0 && !t.supply_on);
        // the status (the hold's countdown) stays current while idle: a second at most
        const TickType_t wait = idle ? pdMS_TO_TICKS(1000)
                                     : (next_us > now ? pdMS_TO_TICKS((next_us - now + 999) / 1000) : 0);
        if (xQueueReceive(g_queue, &msg, wait) == pdTRUE) {
            handle(msg);
            while (xQueueReceive(g_queue, &msg, 0) == pdTRUE) {
                handle(msg);
            }
            if (idle) {
                // the fade counts from now, not from the last frame before the pause
                t.last_frame_us = esp_timer_get_time();
                next_us = t.last_frame_us;
            }
        }
        const int64_t start = esp_timer_get_time();
        if (counted && start > kStableUs) {
            g_crashes = 0;
            counted = false;
        }
        if (start >= next_us) {
            next_us = start + 1000000 / (t.config.fps > 0 ? t.config.fps : 1);
            render_frame(start);
        }
        publish();
    }
}

bool layout_error(const Layout &layout, char *error, size_t error_size)
{
    if (layout.panel_count > kMaxPanels) {
        std::snprintf(error, error_size, "at most %u panels", static_cast<unsigned>(kMaxPanels));
        return true;
    }
    if (layout.panel_count == 0) {
        return false; // no layout: nothing is shown
    }
    auto table = layout::build(layout.width, layout.height, {layout.panels, layout.panel_count});
    if (!table) {
        std::snprintf(error, error_size, "%s", table.error().text);
        return true;
    }
    return false;
}

} // namespace

esp_err_t start(const Start &config)
{
    ESP_RETURN_ON_FALSE(g_queue == nullptr, ESP_ERR_INVALID_STATE, kTag, "already started");
    g_lock = xSemaphoreCreateMutex();
    g_queue = xQueueCreate(16, sizeof(Msg));
    g_gamma = new detail::Gamma();
    ESP_RETURN_ON_FALSE(g_lock != nullptr && g_queue != nullptr, ESP_ERR_NO_MEM, kTag, "queue");
    t.start = config;
    load(kKeyLayout, t.layout);
    load_config_and_styles();
    load(kKeyState, t.state);
    load(kKeyLanes, t.lanes);
    load(kKeyMotion, t.auto_off);
    load(kKeyOverlay, t.overlay);
    t.overlay.text[kOverlayTextSize - 1] = 0;
    if (static_cast<uint8_t>(t.overlay.mode) > static_cast<uint8_t>(OverlayMode::Clock)) {
        t.overlay = {};
    }
    Boot boot{};
    load(kKeyBoot, boot);
    // at power on: as before (off by motion stays so: on with the next motion), off or on
    switch (t.config.power_on) {
    case PowerOn::Off:
        t.state.on = false;
        break;
    case PowerOn::On:
        t.state.on = true;
        break;
    default:
        t.off_by_motion = boot.off_by_motion && !t.state.on && t.auto_off.enabled;
        break;
    }
    if (boot.off_by_motion != t.off_by_motion) {
        save_boot();
    }
    g_pending_styles = t.styles;
    g_pending_overlay = t.overlay;
    if (t.auto_off.minutes < 1 || t.auto_off.minutes > kMaxAutoOffMinutes) {
        t.auto_off = {};
    }
    apply_motion_hold();
    for (LaneSettings &l : t.lanes.lane) {
        if (static_cast<uint8_t>(l.order) > static_cast<uint8_t>(output::ColorOrder::BGR)) {
            l = {};
        }
    }
    // the styles are indexed like the animations
    static_assert(kMaxEffects <= 256);
    if (effects::count() > kMaxEffects) {
        ESP_LOGE(kTag, "more animations than styles (kMaxEffects)");
    }
    if (t.state.effect >= effects::count() || t.state.effect >= kMaxEffects) {
        t.state.effect = 0;
    }
    if (t.layout.panel_count > kMaxPanels) {
        t.layout = {};
    }
    if (t.config.crossfade_ms > kMaxCrossfadeMs || static_cast<uint8_t>(t.config.power_on) > 2) {
        t.config.crossfade_ms = Config{}.crossfade_ms;
        t.config.power_on = PowerOn::Restore;
    }
    const esp_reset_reason_t reason = esp_reset_reason();
    const bool crashed = reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT ||
                         reason == ESP_RST_WDT;
    g_crashes = crashed && g_crash_magic == kCrashMagic ? g_crashes + 1 : (crashed ? 1 : 0);
    g_crash_magic = kCrashMagic;
    if (g_crashes >= kMaxCrashes) {
        ESP_LOGE(kTag, "%u crashes in a row: the LEDs start off, with RMT", static_cast<unsigned>(g_crashes));
        t.state.on = false;
        t.config.driver = output::Driver::Rmt;
    }
    update_mix();
    // the supply is off until the first frame (the board starts with it off)
    if (t.start.supply != nullptr) {
        t.start.supply(false);
    }
    // core 1 on the dual core chips: core 0 has Wi-Fi
    const BaseType_t core = portNUM_PROCESSORS > 1 ? 1 : 0;
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(task, "leds", 4096, nullptr, 5, nullptr, core) == pdPASS,
                        ESP_ERR_NO_MEM, kTag, "task");
    return ESP_OK;
}

bool running()
{
    return g_queue != nullptr;
}

void set_on(bool on)
{
    post(MsgType::On, on ? 1 : 0);
}

void set_brightness(uint8_t brightness)
{
    post(MsgType::Brightness, brightness);
}

void set_effect(size_t index)
{
    if (index < effects::count()) {
        post(MsgType::Effect, static_cast<uint32_t>(index));
    }
}

void set_color(effects::Rgb color)
{
    post(MsgType::Color, (static_cast<uint32_t>(color.r) << 16) | (static_cast<uint32_t>(color.g) << 8) | color.b);
}

void set_speed(uint8_t speed)
{
    post(MsgType::Speed, speed);
}

void set_style(size_t effect, Style style)
{
    if (effect >= effects::count() || effect >= kMaxEffects || g_lock == nullptr) {
        return;
    }
    style.rotation &= 3;
    style.attack_ms = style.attack_ms > effects::kMaxAttackMs ? effects::kMaxAttackMs : style.attack_ms;
    style.release_ms = style.release_ms > effects::kMaxReleaseMs ? effects::kMaxReleaseMs : style.release_ms;
    // all styles wait together: several set at once (the backup) are not lost
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_pending_styles.style[effect] = style;
    xSemaphoreGive(g_lock);
    post(MsgType::Style);
}

Styles styles()
{
    if (g_lock == nullptr) {
        return {};
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    const Styles out = g_styles;
    xSemaphoreGive(g_lock);
    return out;
}

bool set_layout(const Layout &layout, char *error, size_t error_size)
{
    if (g_queue == nullptr) {
        std::snprintf(error, error_size, "the LEDs do not run");
        return false;
    }
    if (layout_error(layout, error, error_size)) {
        return false;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_pending_layout = layout;
    xSemaphoreGive(g_lock);
    post(MsgType::Layout);
    return true;
}

Layout layout()
{
    if (g_lock == nullptr) {
        return {};
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    const Layout out = g_layout;
    xSemaphoreGive(g_lock);
    return out;
}

bool set_config(const Config &config, char *error, size_t error_size)
{
    if (g_queue == nullptr) {
        std::snprintf(error, error_size, "the LEDs do not run");
        return false;
    }
    const char *problem = config.fps < 1 || config.fps > 200      ? "fps: 1 to 200"
                          : config.fade_ms > 10000                ? "fade: 0 to 10000 ms"
                          : config.settle_ms > 2000               ? "settle time: 0 to 2000 ms"
                          : config.channel_mw < 1 || config.channel_mw > 1000 ? "channel: 1 to 1000 mW"
                          : config.idle_mw > 100                  ? "idle: 0 to 100 mW"
                          : config.crossfade_ms > kMaxCrossfadeMs ? "crossfade: 0 to 10000 ms"
                          : static_cast<uint8_t>(config.power_on) > static_cast<uint8_t>(PowerOn::On)
                              ? "power on: restore, off or on"
                              : nullptr;
    if (problem != nullptr) {
        std::snprintf(error, error_size, "%s", problem);
        return false;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_pending_config = config;
    g_pending_config.version = kConfigVersion;
    xSemaphoreGive(g_lock);
    post(MsgType::Config);
    return true;
}

Config config()
{
    if (g_lock == nullptr) {
        return {};
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    const Config out = g_config;
    xSemaphoreGive(g_lock);
    return out;
}

Status status()
{
    if (g_lock == nullptr) {
        Status out{};
        std::snprintf(out.error, sizeof(out.error), "not started");
        return out;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    const Status out = g_status;
    xSemaphoreGive(g_lock);
    return out;
}

void set_lanes(const Lanes &lanes)
{
    if (g_lock == nullptr) {
        return;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_pending_lanes = lanes;
    g_pending_lanes.version = kStoredVersion;
    xSemaphoreGive(g_lock);
    post(MsgType::Lanes);
}

Lanes lanes()
{
    if (g_lock == nullptr) {
        return {};
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    const Lanes out = g_lanes;
    xSemaphoreGive(g_lock);
    return out;
}

void set_overlay(const Overlay &overlay)
{
    if (g_lock == nullptr) {
        return;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_pending_overlay = overlay;
    g_pending_overlay.version = Overlay{}.version;
    g_pending_overlay.text[kOverlayTextSize - 1] = 0;
    xSemaphoreGive(g_lock);
    post(MsgType::Overlay);
}

Overlay overlay()
{
    if (g_lock == nullptr) {
        return {};
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    const Overlay out = g_overlay;
    xSemaphoreGive(g_lock);
    return out;
}

bool preview(uint8_t *out, size_t size, uint16_t &width, uint16_t &height)
{
    if (g_lock == nullptr) {
        return false;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_preview_asked_us = esp_timer_get_time();
    // dark: no frames are drawn, the last one would stay
    const size_t dark = g_status.level == 0 ? static_cast<size_t>(g_status.width) * g_status.height * 3 : 0;
    const bool ok = dark != 0 ? dark <= size : g_preview != nullptr && g_preview_size <= size;
    if (ok && dark != 0) {
        std::memset(out, 0, dark);
        width = g_status.width;
        height = g_status.height;
    } else if (ok) {
        std::memcpy(out, g_preview, g_preview_size);
        width = g_preview_width;
        height = g_preview_height;
    }
    xSemaphoreGive(g_lock);
    return ok;
}

void set_auto_off(const AutoOff &auto_off)
{
    const uint16_t minutes = auto_off.minutes < 1                    ? 1
                             : auto_off.minutes > kMaxAutoOffMinutes ? kMaxAutoOffMinutes
                                                                     : auto_off.minutes;
    post(MsgType::AutoOff, (auto_off.enabled ? 0x10000u : 0u) | minutes);
}

AutoOff auto_off()
{
    if (g_lock == nullptr) {
        return {};
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    const AutoOff out = g_auto_off;
    xSemaphoreGive(g_lock);
    return out;
}

void motion(bool detected)
{
    post(MsgType::Motion, detected ? 1 : 0);
}

void suspend(bool suspended)
{
    post(MsgType::Suspend, suspended ? 1 : 0);
    if (suspended) {
        // the console takes the lanes right after: give the task time to let go of them
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

} // namespace px::leds
