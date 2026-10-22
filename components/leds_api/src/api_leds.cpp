// The LEDs in the web API (docs/ui/web/web-app.md section 4), login required:
//
//   GET  /api/leds            the state, the status of the render task, the animations, the
//                             palettes, the config, the overlay
//   POST /api/leds            any of on=1|0, brightness=0..255, effect=<slug>, color=#rrggbb,
//                             speed=0..255, auto_off=1|0, auto_off_minutes=1..240, and for the
//                             animation (the one given, else the current one) rotation=0|90|180|270,
//                             reactive=1|0, strength=0..255, attack=0..2000, release=0..5000 (ms) and
//                             palette=<slug>; answers like GET
//   GET  /api/presets, POST /api/presets/save|apply|delete: api_presets.cpp
//   GET  /api/leds/layout     {"width", "height", "available_lanes", "panels": [{"lane", "x", "y",
//                             "width", "height", "first": "tl|tr|bl|br", "runs": "rows|columns",
//                             "zigzag"}]}
//   POST /api/leds/layout     width, height, panels: one panel per ';', its fields in the order of
//                             GET separated by ',' ("1,0,0,16,16,tr,rows,zigzag"); 400 with the
//                             problem when the layout cannot be shown
//   GET  /api/leds/lanes      {"lanes": [{"lane", "order", "level", "red", "green", "blue", "dither"}]}
//                             for every lane the board has
//   POST /api/leds/lanes      lane=<1..16> and any of order=grb|rgb|brg|rbg|gbr|bgr, level, red,
//                             green, blue (0..255), dither=1|0; answers like GET
//   POST /api/leds/config     any of power_limit_mw, channel_mw, idle_mw, fps, fade_ms, settle_ms,
//                             crossfade_ms, driver=rmt|parallel, dither=1|0,
//                             power_on=restore|off|on
//   POST /api/leds/overlay    any of mode=off|text|clock, text (UTF-8, 63 bytes), color=#rrggbb,
//                             speed=0..255, clock_24h=1|0; answers like GET /api/leds
//   GET  /api/leds/live       a WebSocket: binary messages of the canvas as drawn (before the
//                             brightness), 4 bytes width and height (16 bits each, little endian),
//                             then RGB row by row; 10 a second, 5 for a canvas over 4096 pixels.
//                             With an audio input 20 a second, the ones between with 0 x 0 (no
//                             pixels); every message then ends with the sound: flags (1 active,
//                             2 beat), loudness, the 32 bands
//
// Only in the main firmware: main registers the routes (webserver::Config::add_routes).

#include "px/leds_api/leds_api.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "px/effects/effects.hpp"
#include "px/leds/leds.hpp"
#include "px/memory/large.hpp"
#include "px/webserver/http.hpp"

#include "api_presets.hpp"

namespace px::leds_api {

namespace {

using webserver::detail::form_has;
using webserver::detail::form_value;
using webserver::detail::JsonWriter;
using webserver::detail::logged_in;
using webserver::detail::read_body;
using webserver::detail::require_login;
using webserver::detail::route;
using webserver::detail::send_error;
using webserver::detail::send_json;

constexpr const char *kCorners[] = {"tl", "tr", "bl", "br"};
constexpr const char *kOrders[] = {"grb", "rgb", "brg", "rbg", "gbr", "bgr"};
constexpr const char *kPowerOn[] = {"restore", "off", "on"};
constexpr const char *kOverlayModes[] = {"off", "text", "clock"};

// the index of `text` in `names`, `count` when it is none of them
size_t choice(const char *text, const char *const *names, size_t count)
{
    size_t i = 0;
    while (i < count && std::strcmp(text, names[i]) != 0) {
        i++;
    }
    return i;
}

void color_text(effects::Rgb c, char (&out)[8])
{
    std::snprintf(out, sizeof(out), "#%02x%02x%02x", c.r, c.g, c.b);
}

bool parse_color(const char *text, effects::Rgb &out)
{
    if (text[0] != '#' || std::strlen(text) != 7) {
        return false;
    }
    char *end = nullptr;
    const unsigned long value = std::strtoul(text + 1, &end, 16);
    if (*end != 0) {
        return false;
    }
    out = {static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
    return true;
}

// a whole number from `min` to `max` in the form; false (and 400 sent) when it is there but wrong
bool form_number(httpd_req_t *req, const char *body, const char *key, long min, long max, long &out, bool &present)
{
    char text[16] = {};
    present = form_has(body, key);
    if (!present) {
        return true;
    }
    char *end = nullptr;
    if (!form_value(body, key, text, sizeof(text)) || (out = std::strtol(text, &end, 10), *end != 0) || text[0] == 0 ||
        out < min || out > max) {
        char message[64];
        std::snprintf(message, sizeof(message), "%s: a whole number from %ld to %ld", key, min, max);
        send_error(req, "400 Bad Request", message);
        return false;
    }
    return true;
}

esp_err_t send_state(httpd_req_t *req)
{
    const leds::Status s = leds::status();
    const leds::Config c = leds::config();
    const leds::Overlay o = leds::overlay();
    const effects::Info *effect = effects::info(s.state.effect);
    char color[8];
    color_text(s.state.color, color);
    char overlay_color[8];
    color_text(o.color, overlay_color);
    static char buffer[4096]; // the server has one task
    JsonWriter json(buffer, sizeof(buffer));
    json.begin_object()
        .boolean("on", s.state.on)
        .number("brightness", s.state.brightness)
        .number("level", s.level)
        .string("effect", effect != nullptr ? effect->slug : "")
        .string("color", color)
        .number("speed", s.state.speed)
        .number("rotation", s.style.rotation * 90)
        .boolean("reactive", s.style.reactive)
        .number("strength", s.style.strength)
        .number("attack", s.style.attack_ms)
        .number("release", s.style.release_ms)
        .string("palette", effects::palette_info(s.style.palette) != nullptr ? effects::palette_info(s.style.palette)->slug
                                                                              : "none")
        .begin_object("audio")
        .boolean("source", s.has_audio)
        .boolean("hears", s.hears_audio)
        .end_object()
        .boolean("supply_on", s.supply_on)
        .number("fps", s.fps)
        .number("render_us", s.render_us)
        .number("compose_us", s.compose_us)
        .number("send_us", s.send_us)
        .number("power_mw", s.power_mw)
        .boolean("limited", s.limited)
        .string("error", s.error)
        .begin_object("motion")
        .boolean("sensor", s.has_motion)
        .boolean("detected", s.motion)
        .boolean("auto_off", leds::auto_off().enabled)
        .number("minutes", leds::auto_off().minutes)
        .number("off_in_s", s.auto_off_in_s)
        .boolean("off_by_motion", s.off_by_motion)
        .end_object()
        .number("width", s.width)
        .number("height", s.height)
        .number("leds", s.leds)
        .number("lanes", s.lanes)
        .begin_array("effects");
    for (size_t i = 0; i < effects::count(); i++) {
        const effects::Info *e = effects::info(i);
        json.begin_object()
            .string("slug", e->slug)
            .string("name", e->name)
            .boolean("color", e->uses_color)
            .boolean("palette", e->uses_palette)
            .string("reactive", e->reactive == effects::Reactive::Never      ? "never"
                                : e->reactive == effects::Reactive::Optional ? "optional"
                                                                              : "required")
            .end_object();
    }
    json.end_array().begin_array("palettes");
    for (size_t i = 0; i < effects::palette_count(); i++) {
        json.begin_object()
            .string("slug", effects::palette_info(i)->slug)
            .string("name", effects::palette_info(i)->name)
            .end_object();
    }
    json.end_array()
        .begin_object("config")
        .number("power_limit_mw", c.power_limit_mw)
        .number("channel_mw", c.channel_mw)
        .number("idle_mw", c.idle_mw)
        .number("fps", c.fps)
        .number("fade_ms", c.fade_ms)
        .number("settle_ms", c.settle_ms)
        .number("crossfade_ms", c.crossfade_ms)
        .string("power_on", kPowerOn[static_cast<size_t>(c.power_on) % std::size(kPowerOn)])
        .string("driver", c.driver == output::Driver::Parallel ? "parallel" : "rmt")
        .boolean("dither", c.dither)
        .boolean("parallel_supported", output::parallel_supported())
        .end_object()
        .begin_object("overlay")
        .string("mode", kOverlayModes[static_cast<size_t>(o.mode) % std::size(kOverlayModes)])
        .string("text", o.text)
        .string("color", overlay_color)
        .number("speed", o.speed)
        .boolean("clock_24h", o.clock_24h)
        .end_object()
        .end_object();
    return send_json(req, json);
}

esp_err_t get_leds(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    return send_state(req);
}

esp_err_t post_leds(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    char body[256];
    if (!read_body(req, body, sizeof(body))) {
        return send_error(req, "400 Bad Request", "the request is too large");
    }
    long brightness = 0, speed = 0, minutes = 0, rotation = 0, strength = 0, attack = 0, release = 0;
    bool has_brightness = false, has_speed = false, has_minutes = false, has_rotation = false, has_strength = false,
         has_attack = false, has_release = false;
    if (!form_number(req, body, "brightness", 0, 255, brightness, has_brightness) ||
        !form_number(req, body, "speed", 0, 255, speed, has_speed) ||
        !form_number(req, body, "rotation", 0, 270, rotation, has_rotation) ||
        !form_number(req, body, "strength", 0, 255, strength, has_strength) ||
        !form_number(req, body, "attack", 0, effects::kMaxAttackMs, attack, has_attack) ||
        !form_number(req, body, "release", 0, effects::kMaxReleaseMs, release, has_release) ||
        !form_number(req, body, "auto_off_minutes", 1, leds::kMaxAutoOffMinutes, minutes, has_minutes)) {
        return ESP_OK;
    }
    if (has_rotation && rotation % 90 != 0) {
        return send_error(req, "400 Bad Request", "rotation: 0, 90, 180 or 270");
    }
    char reactive_text[4] = {};
    const bool has_reactive = form_value(body, "reactive", reactive_text, sizeof(reactive_text));
    if (has_reactive && std::strcmp(reactive_text, "1") != 0 && std::strcmp(reactive_text, "0") != 0) {
        return send_error(req, "400 Bad Request", "reactive: 1 or 0");
    }
    char auto_off_text[4] = {};
    const bool has_auto_off = form_value(body, "auto_off", auto_off_text, sizeof(auto_off_text));
    if (has_auto_off && std::strcmp(auto_off_text, "1") != 0 && std::strcmp(auto_off_text, "0") != 0) {
        return send_error(req, "400 Bad Request", "auto_off: 1 or 0");
    }
    char text[32] = {};
    size_t effect = effects::count();
    if (form_has(body, "effect")) {
        if (!form_value(body, "effect", text, sizeof(text)) || (effect = effects::find(text)) >= effects::count()) {
            return send_error(req, "400 Bad Request", "effect: not an animation of this firmware");
        }
    }
    size_t palette = effects::palette_count();
    const bool has_palette = form_has(body, "palette");
    if (has_palette &&
        (!form_value(body, "palette", text, sizeof(text)) || (palette = effects::find_palette(text)) >= effects::palette_count())) {
        return send_error(req, "400 Bad Request", "palette: not a palette of this firmware");
    }
    effects::Rgb color{};
    const bool has_color = form_has(body, "color");
    if (has_color && (!form_value(body, "color", text, sizeof(text)) || !parse_color(text, color))) {
        return send_error(req, "400 Bad Request", "color: #rrggbb");
    }
    bool on = false;
    const bool has_on = form_value(body, "on", text, sizeof(text));
    if (has_on) {
        if (std::strcmp(text, "1") != 0 && std::strcmp(text, "0") != 0) {
            return send_error(req, "400 Bad Request", "on: 1 or 0");
        }
        on = text[0] == '1';
    }
    // all checked: now applied, in the order a person would do it
    if (effect < effects::count()) {
        leds::set_effect(effect);
    }
    if (has_rotation || has_reactive || has_strength || has_attack || has_release || has_palette) {
        const size_t target = effect < effects::count() ? effect : leds::status().state.effect;
        leds::Style style = leds::styles().style[target < leds::kMaxEffects ? target : 0];
        if (has_rotation) {
            style.rotation = static_cast<uint8_t>(rotation / 90);
        }
        if (has_reactive) {
            style.reactive = reactive_text[0] == '1';
        }
        if (has_strength) {
            style.strength = static_cast<uint8_t>(strength);
        }
        if (has_attack) {
            style.attack_ms = static_cast<uint16_t>(attack);
        }
        if (has_release) {
            style.release_ms = static_cast<uint16_t>(release);
        }
        if (has_palette) {
            style.palette = static_cast<uint8_t>(palette);
        }
        leds::set_style(target, style);
    }
    if (has_color) {
        leds::set_color(color);
    }
    if (has_speed) {
        leds::set_speed(static_cast<uint8_t>(speed));
    }
    if (has_brightness) {
        leds::set_brightness(static_cast<uint8_t>(brightness));
    }
    if (has_on) {
        leds::set_on(on);
    }
    if (has_auto_off || has_minutes) {
        leds::AutoOff a = leds::auto_off();
        if (has_auto_off) {
            a.enabled = auto_off_text[0] == '1';
        }
        if (has_minutes) {
            a.minutes = static_cast<uint16_t>(minutes);
        }
        leds::set_auto_off(a);
    }
    // the task applies them with its next loop
    vTaskDelay(pdMS_TO_TICKS(30));
    return send_state(req);
}

esp_err_t get_layout(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    const leds::Layout layout = leds::layout();
    const leds::Status s = leds::status();
    static char buffer[2048];
    JsonWriter json(buffer, sizeof(buffer));
    json.begin_object()
        .number("width", layout.width)
        .number("height", layout.height)
        .number("available_lanes", s.available_lanes)
        .number("max_panels", leds::kMaxPanels)
        .begin_array("panels");
    for (size_t i = 0; i < layout.panel_count; i++) {
        const layout::Panel &p = layout.panels[i];
        json.begin_object()
            .number("lane", p.lane + 1)
            .number("x", p.x)
            .number("y", p.y)
            .number("width", p.width)
            .number("height", p.height)
            .string("first", kCorners[static_cast<size_t>(p.first)])
            .string("runs", p.columns ? "columns" : "rows")
            .boolean("zigzag", p.zigzag)
            .end_object();
    }
    json.end_array().end_object();
    return send_json(req, json);
}

// "1,0,0,16,16,tr,rows,zigzag"
bool parse_panel(char *text, layout::Panel &out)
{
    char *fields[8] = {};
    size_t count = 0;
    for (char *save = nullptr, *field = strtok_r(text, ",", &save); field != nullptr && count < 8;
         field = strtok_r(nullptr, ",", &save)) {
        fields[count++] = field;
    }
    if (count != 8) {
        return false;
    }
    long numbers[5];
    for (size_t i = 0; i < 5; i++) {
        char *end = nullptr;
        numbers[i] = std::strtol(fields[i], &end, 10);
        if (*end != 0 || numbers[i] < 0 || numbers[i] > 8192) {
            return false;
        }
    }
    size_t corner = 0;
    while (corner < 4 && std::strcmp(fields[5], kCorners[corner]) != 0) {
        corner++;
    }
    const bool columns = std::strcmp(fields[6], "columns") == 0;
    const bool zigzag = std::strcmp(fields[7], "zigzag") == 0;
    if (numbers[0] < 1 || numbers[0] > 16 || corner == 4 || (!columns && std::strcmp(fields[6], "rows") != 0) ||
        (!zigzag && std::strcmp(fields[7], "straight") != 0)) {
        return false;
    }
    out = {.lane = static_cast<uint8_t>(numbers[0] - 1),
           .x = static_cast<uint16_t>(numbers[1]),
           .y = static_cast<uint16_t>(numbers[2]),
           .width = static_cast<uint16_t>(numbers[3]),
           .height = static_cast<uint16_t>(numbers[4]),
           .first = static_cast<layout::Corner>(corner),
           .columns = columns,
           .zigzag = zigzag};
    return true;
}

esp_err_t post_layout(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    static char body[1024];
    if (!read_body(req, body, sizeof(body))) {
        return send_error(req, "400 Bad Request", "the request is too large");
    }
    long width = 0, height = 0;
    bool has_width = false, has_height = false;
    if (!form_number(req, body, "width", 1, 8192, width, has_width) ||
        !form_number(req, body, "height", 1, 8192, height, has_height)) {
        return ESP_OK;
    }
    static char panels[768];
    if (!has_width || !has_height || !form_value(body, "panels", panels, sizeof(panels))) {
        return send_error(req, "400 Bad Request", "width, height and panels are needed");
    }
    leds::Layout layout;
    layout.width = static_cast<uint16_t>(width);
    layout.height = static_cast<uint16_t>(height);
    for (char *save = nullptr, *text = strtok_r(panels, ";", &save); text != nullptr;
         text = strtok_r(nullptr, ";", &save)) {
        if (layout.panel_count >= leds::kMaxPanels) {
            return send_error(req, "400 Bad Request", "at most 16 panels");
        }
        if (!parse_panel(text, layout.panels[layout.panel_count])) {
            char message[96];
            std::snprintf(message, sizeof(message), "panel %u: lane,x,y,width,height,tl|tr|bl|br,rows|columns,zigzag|straight",
                          layout.panel_count + 1u);
            return send_error(req, "400 Bad Request", message);
        }
        layout.panel_count++;
    }
    char error[96] = {};
    if (!leds::set_layout(layout, error, sizeof(error))) {
        return send_error(req, "400 Bad Request", error);
    }
    // the task stores and applies it with its next loop
    vTaskDelay(pdMS_TO_TICKS(50));
    return get_layout(req);
}

esp_err_t post_config(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    char body[256];
    if (!read_body(req, body, sizeof(body))) {
        return send_error(req, "400 Bad Request", "the request is too large");
    }
    leds::Config c = leds::config();
    long limit = c.power_limit_mw, channel = c.channel_mw, idle = c.idle_mw, fps = c.fps, fade = c.fade_ms,
         settle = c.settle_ms, crossfade = c.crossfade_ms;
    bool present = false;
    if (!form_number(req, body, "power_limit_mw", 0, 10000000, limit, present) ||
        !form_number(req, body, "channel_mw", 1, 1000, channel, present) ||
        !form_number(req, body, "idle_mw", 0, 100, idle, present) || !form_number(req, body, "fps", 1, 200, fps, present) ||
        !form_number(req, body, "fade_ms", 0, 10000, fade, present) ||
        !form_number(req, body, "settle_ms", 0, 2000, settle, present) ||
        !form_number(req, body, "crossfade_ms", 0, leds::kMaxCrossfadeMs, crossfade, present)) {
        return ESP_OK;
    }
    c.crossfade_ms = static_cast<uint16_t>(crossfade);
    char power_on[12] = {};
    if (form_value(body, "power_on", power_on, sizeof(power_on))) {
        const size_t index = choice(power_on, kPowerOn, std::size(kPowerOn));
        if (index == std::size(kPowerOn)) {
            return send_error(req, "400 Bad Request", "power_on: restore, off or on");
        }
        c.power_on = static_cast<leds::PowerOn>(index);
    }
    c.power_limit_mw = static_cast<uint32_t>(limit);
    c.channel_mw = static_cast<uint16_t>(channel);
    c.idle_mw = static_cast<uint16_t>(idle);
    c.fps = static_cast<uint8_t>(fps);
    c.fade_ms = static_cast<uint16_t>(fade);
    c.settle_ms = static_cast<uint16_t>(settle);
    char dither[4] = {};
    if (form_value(body, "dither", dither, sizeof(dither))) {
        if (std::strcmp(dither, "1") != 0 && std::strcmp(dither, "0") != 0) {
            return send_error(req, "400 Bad Request", "dither: 1 or 0");
        }
        c.dither = dither[0] == '1';
    }
    char driver[16] = {};
    if (form_value(body, "driver", driver, sizeof(driver))) {
        if (std::strcmp(driver, "parallel") != 0 && std::strcmp(driver, "rmt") != 0) {
            return send_error(req, "400 Bad Request", "driver: rmt or parallel");
        }
        c.driver = driver[0] == 'p' ? output::Driver::Parallel : output::Driver::Rmt;
    }
    char error[64] = {};
    if (!leds::set_config(c, error, sizeof(error))) {
        return send_error(req, "400 Bad Request", error);
    }
    vTaskDelay(pdMS_TO_TICKS(30));
    return send_state(req);
}

esp_err_t post_overlay(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    char body[512];
    if (!read_body(req, body, sizeof(body))) {
        return send_error(req, "400 Bad Request", "the request is too large");
    }
    leds::Overlay o = leds::overlay();
    long speed = o.speed;
    bool present = false;
    if (!form_number(req, body, "speed", 0, 255, speed, present)) {
        return ESP_OK;
    }
    o.speed = static_cast<uint8_t>(speed);
    char text[leds::kOverlayTextSize + 1] = {};
    if (form_value(body, "mode", text, sizeof(text))) {
        const size_t index = choice(text, kOverlayModes, std::size(kOverlayModes));
        if (index == std::size(kOverlayModes)) {
            return send_error(req, "400 Bad Request", "mode: off, text or clock");
        }
        o.mode = static_cast<leds::OverlayMode>(index);
    }
    if (form_has(body, "text")) {
        if (!form_value(body, "text", text, leds::kOverlayTextSize)) {
            return send_error(req, "400 Bad Request", "text: at most 63 bytes");
        }
        std::snprintf(o.text, sizeof(o.text), "%s", text);
    }
    if (form_has(body, "color") && (!form_value(body, "color", text, sizeof(text)) || !parse_color(text, o.color))) {
        return send_error(req, "400 Bad Request", "color: #rrggbb");
    }
    if (form_value(body, "clock_24h", text, sizeof(text))) {
        if (std::strcmp(text, "1") != 0 && std::strcmp(text, "0") != 0) {
            return send_error(req, "400 Bad Request", "clock_24h: 1 or 0");
        }
        o.clock_24h = text[0] == '1';
    }
    leds::set_overlay(o);
    vTaskDelay(pdMS_TO_TICKS(30));
    return send_state(req);
}

// ------------------------------------------------------------------------- the live preview

// The WebSockets of /api/leds/live: a timer queues a send on the server's task, which copies the
// canvas (leds::preview) and the sound and sends them to each of them. The server's own task
// only: no lock.
constexpr size_t kMaxLive = 4;
// the sound at the end of a message: flags, loudness, the bands
constexpr size_t kAudioSize = 2 + effects::kBands;
httpd_handle_t g_server = nullptr;
int g_live[kMaxLive] = {-1, -1, -1, -1};
esp_timer_handle_t g_live_timer = nullptr;
uint32_t g_live_ticks = 0;
uint8_t *g_live_buffer = nullptr;
size_t g_live_size = 0;
void (*g_audio)(effects::Audio &out) = nullptr;

// the sound after `length` bytes of the message; the new length
size_t add_audio(size_t length)
{
    if (g_audio == nullptr) {
        return length;
    }
    effects::Audio audio{};
    g_audio(audio);
    uint8_t *out = g_live_buffer + length;
    out[0] = static_cast<uint8_t>((audio.active ? 1 : 0) | (audio.beat ? 2 : 0));
    out[1] = audio.loudness;
    std::memcpy(out + 2, audio.bands, effects::kBands);
    return length + kAudioSize;
}

void send_frame(size_t length)
{
    httpd_ws_frame_t frame{};
    frame.type = HTTPD_WS_TYPE_BINARY;
    frame.payload = g_live_buffer;
    frame.len = length;
    for (int &fd : g_live) {
        if (fd >= 0 && httpd_ws_send_frame_async(g_server, fd, &frame) != ESP_OK) {
            fd = -1;
        }
    }
}

void send_live(void *)
{
    size_t open = 0;
    for (int &fd : g_live) {
        if (fd >= 0 && httpd_ws_get_fd_info(g_server, fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
            fd = -1; // closed, or the number taken by another connection
        }
        open += fd >= 0 ? 1 : 0;
    }
    if (open == 0) {
        esp_timer_stop(g_live_timer);
        memory::release_large(g_live_buffer);
        g_live_buffer = nullptr;
        g_live_size = 0;
        return;
    }
    const leds::Status s = leds::status();
    const size_t pixels = static_cast<size_t>(s.width) * s.height;
    // the canvas every other tick (10 a second), a large one every fourth; the sound every tick
    bool with_pixels = pixels > 0 && g_live_ticks++ % (pixels > 4096 ? 4 : 2) == 0;
    if (!with_pixels && g_audio == nullptr) {
        return;
    }
    const size_t size = 4 + pixels * 3 + kAudioSize;
    if (g_live_size < size) {
        memory::release_large(g_live_buffer);
        g_live_buffer = static_cast<uint8_t *>(memory::allocate_large(size));
        g_live_size = g_live_buffer != nullptr ? size : 0;
        if (g_live_buffer == nullptr) {
            return;
        }
    }
    uint16_t width = 0, height = 0;
    if (with_pixels && !leds::preview(g_live_buffer + 4, g_live_size - 4 - kAudioSize, width, height)) {
        // the task copies from its next frame on
        with_pixels = false;
        width = height = 0;
        if (g_audio == nullptr) {
            return;
        }
    }
    g_live_buffer[0] = static_cast<uint8_t>(width);
    g_live_buffer[1] = static_cast<uint8_t>(width >> 8);
    g_live_buffer[2] = static_cast<uint8_t>(height);
    g_live_buffer[3] = static_cast<uint8_t>(height >> 8);
    send_frame(add_audio(4 + static_cast<size_t>(width) * height * 3));
}

void on_live_timer(void *)
{
    httpd_queue_work(g_server, send_live, nullptr);
}

// ESP-IDF 6.1 answers the handshake without calling the route's handler: the login is checked
// before it (refused: no socket), the watcher is taken after it
esp_err_t live_pre_handshake(httpd_req_t *req)
{
    return logged_in(req) ? ESP_OK : ESP_FAIL;
}

esp_err_t live_post_handshake(httpd_req_t *req)
{
    const int fd = httpd_req_to_sockfd(req);
    int *slot = nullptr;
    for (int &f : g_live) {
        slot = slot == nullptr && (f < 0 || f == fd) ? &f : slot;
    }
    if (slot == nullptr) {
        return ESP_FAIL; // enough watchers
    }
    *slot = fd;
    g_server = req->handle;
    if (g_live_timer == nullptr) {
        esp_timer_create_args_t args{};
        args.callback = on_live_timer;
        args.name = "leds_live";
        if (esp_timer_create(&args, &g_live_timer) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    if (!esp_timer_is_active(g_live_timer)) {
        esp_timer_start_periodic(g_live_timer, 50 * 1000);
    }
    return ESP_OK;
}

esp_err_t live(httpd_req_t *req)
{
    // the client sends nothing the server needs: read and drop it
    httpd_ws_frame_t frame{};
    if (httpd_ws_recv_frame(req, &frame, 0) != ESP_OK) {
        return ESP_FAIL;
    }
    if (frame.len > 0 && frame.len <= 64) {
        uint8_t ignored[64];
        frame.payload = ignored;
        httpd_ws_recv_frame(req, &frame, frame.len);
    }
    return ESP_OK;
}

esp_err_t send_lanes(httpd_req_t *req)
{
    const leds::Lanes lanes = leds::lanes();
    const leds::Status s = leds::status();
    static char buffer[1536];
    JsonWriter json(buffer, sizeof(buffer));
    json.begin_object().begin_array("lanes");
    for (size_t i = 0; i < s.available_lanes; i++) {
        const leds::LaneSettings &l = lanes.lane[i];
        json.begin_object()
            .number("lane", static_cast<int64_t>(i + 1))
            .string("order", kOrders[static_cast<size_t>(l.order)])
            .number("level", l.level)
            .number("red", l.red)
            .number("green", l.green)
            .number("blue", l.blue)
            .boolean("dither", l.dither)
            .end_object();
    }
    json.end_array().end_object();
    return send_json(req, json);
}

esp_err_t get_lanes(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    return send_lanes(req);
}

esp_err_t post_lanes(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    char body[256];
    if (!read_body(req, body, sizeof(body))) {
        return send_error(req, "400 Bad Request", "the request is too large");
    }
    long lane = 0, level = 0, red = 0, green = 0, blue = 0;
    bool has_lane = false, has_level = false, has_red = false, has_green = false, has_blue = false;
    if (!form_number(req, body, "lane", 1, static_cast<long>(output::kMaxLanes), lane, has_lane) ||
        !form_number(req, body, "level", 0, 255, level, has_level) ||
        !form_number(req, body, "red", 0, 255, red, has_red) ||
        !form_number(req, body, "green", 0, 255, green, has_green) ||
        !form_number(req, body, "blue", 0, 255, blue, has_blue)) {
        return ESP_OK;
    }
    if (!has_lane) {
        return send_error(req, "400 Bad Request", "lane is needed");
    }
    leds::Lanes lanes = leds::lanes();
    leds::LaneSettings &l = lanes.lane[lane - 1];
    char text[8] = {};
    if (form_value(body, "order", text, sizeof(text))) {
        size_t order = 0;
        while (order < std::size(kOrders) && std::strcmp(text, kOrders[order]) != 0) {
            order++;
        }
        if (order == std::size(kOrders)) {
            return send_error(req, "400 Bad Request", "order: grb, rgb, brg, rbg, gbr or bgr");
        }
        l.order = static_cast<output::ColorOrder>(order);
    }
    if (form_value(body, "dither", text, sizeof(text))) {
        if (std::strcmp(text, "1") != 0 && std::strcmp(text, "0") != 0) {
            return send_error(req, "400 Bad Request", "dither: 1 or 0");
        }
        l.dither = text[0] == '1';
    }
    if (has_level) {
        l.level = static_cast<uint8_t>(level);
    }
    if (has_red) {
        l.red = static_cast<uint8_t>(red);
    }
    if (has_green) {
        l.green = static_cast<uint8_t>(green);
    }
    if (has_blue) {
        l.blue = static_cast<uint8_t>(blue);
    }
    leds::set_lanes(lanes);
    vTaskDelay(pdMS_TO_TICKS(30));
    return send_lanes(req);
}

} // namespace

void register_routes(void *server, void (*audio)(effects::Audio &out))
{
    g_audio = audio;
    const httpd_uri_t routes[] = {
        route("/api/leds", HTTP_GET, get_leds),
        route("/api/leds", HTTP_POST, post_leds),
        route("/api/leds/layout", HTTP_GET, get_layout),
        route("/api/leds/layout", HTTP_POST, post_layout),
        route("/api/leds/config", HTTP_POST, post_config),
        route("/api/leds/lanes", HTTP_GET, get_lanes),
        route("/api/leds/lanes", HTTP_POST, post_lanes),
        route("/api/leds/overlay", HTTP_POST, post_overlay),
    };
    for (const auto &r : routes) {
        httpd_register_uri_handler(static_cast<httpd_handle_t>(server), &r);
    }
    httpd_uri_t live_route = route("/api/leds/live", HTTP_GET, live);
    live_route.is_websocket = true;
    live_route.ws_pre_handshake_cb = live_pre_handshake;
    live_route.ws_post_handshake_cb = live_post_handshake;
    httpd_register_uri_handler(static_cast<httpd_handle_t>(server), &live_route);
    register_preset_routes(server);
}

} // namespace px::leds_api
