// MQTT and Home Assistant discovery (px/hass/hass.hpp, docs/leds.md section 8).

#include "px/hass/hass.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iterator>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mqtt_client.h"
#include "px/effects/effects.hpp"
#include "px/formats/json_writer.hpp"
#include "px/leds/leds.hpp"
#include "px/platform/settings.hpp"

namespace px::hass {

namespace {

constexpr const char *kTag = "mqtt";
constexpr const char *kNamespace = "mqtt";
constexpr const char *kKeySettings = "settings";
constexpr const char *kKeyPassword = "password";
// how often the state is compared with the last one sent, and how often the sensors go out
constexpr uint64_t kCheckUs = 250 * 1000;
constexpr int64_t kSensorUs = 30 * 1000000;

SemaphoreHandle_t g_lock = nullptr;
Device g_device = {};
Settings g_settings = {};
char g_password[kPasswordSize] = {};
// the node id in Home Assistant: the device name with '-' as '_'
char g_node[40] = {};
char g_base[kTopicSize] = {};
esp_mqtt_client_handle_t g_client = nullptr;
Status g_status = {};
int64_t g_connected_at = 0;
esp_timer_handle_t g_timer = nullptr;

// what was sent last (the timer's task only), to send changes
struct Sent {
    bool valid = false;
    leds::State state;
    int64_t sensors_at = 0;
    bool motion_valid = false;
    bool motion = false;
    bool auto_off_valid = false;
    leds::AutoOff auto_off;
    bool style_valid = false;
    leds::Style style;
    bool audio_valid = false;
    bool audio = false;
    bool overlay_valid = false;
    leds::Overlay overlay;
    bool power_on_valid = false;
    leds::PowerOn power_on = leds::PowerOn::Restore;
};
Sent g_sent;
bool g_motion = false;
bool g_motion_known = false;

void set_status(State state, const char *error)
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_status.state = state;
    if (error != nullptr) {
        std::snprintf(g_status.error, sizeof(g_status.error), "%s", error);
    }
    xSemaphoreGive(g_lock);
}

void topic(char (&out)[kTopicSize * 2], const char *suffix)
{
    std::snprintf(out, sizeof(out), "%s/%s", g_base, suffix);
}

// g_client is set and cleared under g_lock: the web server's task replaces the client while the
// timer and the client's own task publish
void publish(const char *topic_name, const char *payload, bool retain)
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_client != nullptr) {
        // into the outbox, the client's task sends it: never blocks the caller on the network
        esp_mqtt_client_enqueue(g_client, topic_name, payload, 0, 1, retain ? 1 : 0, true);
    }
    xSemaphoreGive(g_lock);
}

// ------------------------------------------------------------------------------- discovery

constexpr const char *kPowerOn[] = {"restore", "off", "on"};
constexpr const char *kOverlayModes[] = {"off", "text", "clock"};

bool has_audio()
{
    return g_device.has_audio != nullptr && g_device.has_audio();
}

// the common part of every config: names, ids, availability, the device
void begin_config(formats::JsonWriter &json, const char *object, const char *name)
{
    char unique[64];
    std::snprintf(unique, sizeof(unique), "%s_%s", g_node, object);
    char availability[kTopicSize * 2];
    topic(availability, "availability");
    char url[64];
    std::snprintf(url, sizeof(url), "http://%s.local/", g_device.name);
    json.begin_object()
        .string("name", name)
        .string("unique_id", unique)
        .string("availability_topic", availability)
        .begin_object("device")
        .begin_array("identifiers")
        .string(nullptr, g_node)
        .end_array()
        .string("name", g_device.name)
        .string("manufacturer", "PixeLED")
        .string("model", g_device.board)
        .string("sw_version", g_device.firmware)
        .string("configuration_url", url)
        .end_object();
}

void announce(const char *component, const char *object, formats::JsonWriter &json)
{
    if (!json.ok()) {
        ESP_LOGE(kTag, "the config of %s does not fit", object);
        return;
    }
    char config_topic[kTopicSize * 3];
    std::snprintf(config_topic, sizeof(config_topic), "%s/%s/%s/%s/config", g_settings.prefix, component, g_node,
                  object);
    publish(config_topic, json.c_str(), true);
}

// an entity that is not there (any more): its config emptied, Home Assistant removes it
void forget(const char *component, const char *object)
{
    char config_topic[kTopicSize * 3];
    std::snprintf(config_topic, sizeof(config_topic), "%s/%s/%s/%s/config", g_settings.prefix, component, g_node,
                  object);
    publish(config_topic, "", true);
}

// a number entity on <object>/set and <object>/state
void announce_number(char *buffer, size_t size, const char *object, const char *name, int min, int max,
                     const char *unit, const char *icon, bool config)
{
    char command[kTopicSize * 2], state[kTopicSize * 2], suffix[40];
    std::snprintf(suffix, sizeof(suffix), "%s/set", object);
    topic(command, suffix);
    std::snprintf(suffix, sizeof(suffix), "%s/state", object);
    topic(state, suffix);
    formats::JsonWriter json(buffer, size);
    begin_config(json, object, name);
    json.string("command_topic", command)
        .string("state_topic", state)
        .number("min", min)
        .number("max", max)
        .string("mode", "slider")
        .string("icon", icon);
    if (unit != nullptr) {
        json.string("unit_of_measurement", unit);
    }
    if (config) {
        json.string("entity_category", "config");
    }
    json.end_object();
    announce("number", object, json);
}

// a select entity on <object>/set and <object>/state
void announce_select(char *buffer, size_t size, const char *object, const char *name, const char *const *options,
                     size_t count, const char *icon, bool config)
{
    char command[kTopicSize * 2], state[kTopicSize * 2], suffix[40];
    std::snprintf(suffix, sizeof(suffix), "%s/set", object);
    topic(command, suffix);
    std::snprintf(suffix, sizeof(suffix), "%s/state", object);
    topic(state, suffix);
    formats::JsonWriter json(buffer, size);
    begin_config(json, object, name);
    json.string("command_topic", command).string("state_topic", state).begin_array("options");
    for (size_t i = 0; i < count; i++) {
        json.string(nullptr, options[i]);
    }
    json.end_array().string("icon", icon);
    if (config) {
        json.string("entity_category", "config");
    }
    json.end_object();
    announce("select", object, json);
}

void announce_all()
{
    static char buffer[1536]; // the client's task only
    char command[kTopicSize * 2], state[kTopicSize * 2];
    const bool audio = has_audio();

    {
        formats::JsonWriter json(buffer, sizeof(buffer));
        begin_config(json, "light", "LEDs");
        topic(command, "light/set");
        topic(state, "light/state");
        json.string("schema", "json")
            .string("command_topic", command)
            .string("state_topic", state)
            .boolean("brightness", true)
            .begin_array("supported_color_modes")
            .string(nullptr, "rgb")
            .end_array()
            .boolean("effect", true)
            .begin_array("effect_list");
        for (size_t i = 0; i < effects::count(); i++) {
            // an animation that needs sound only with a source
            if (audio || effects::info(i)->reactive != effects::Reactive::Required) {
                json.string(nullptr, effects::info(i)->name);
            }
        }
        json.end_array().end_object();
        announce("light", "light", json);
    }
    {
        formats::JsonWriter json(buffer, sizeof(buffer));
        begin_config(json, "speed", "Speed");
        topic(command, "speed/set");
        topic(state, "speed/state");
        json.string("command_topic", command)
            .string("state_topic", state)
            .number("min", 0)
            .number("max", 255)
            .string("mode", "slider")
            .string("icon", "mdi:speedometer")
            .end_object();
        announce("number", "speed", json);
    }
    topic(state, "sensors");
    {
        formats::JsonWriter json(buffer, sizeof(buffer));
        begin_config(json, "power", "Power");
        json.string("state_topic", state)
            .string("value_template", "{{ value_json.power }}")
            .string("unit_of_measurement", "W")
            .string("device_class", "power")
            .string("state_class", "measurement")
            .end_object();
        announce("sensor", "power", json);
    }
    {
        formats::JsonWriter json(buffer, sizeof(buffer));
        begin_config(json, "fps", "Frames per second");
        json.string("state_topic", state)
            .string("value_template", "{{ value_json.fps }}")
            .string("unit_of_measurement", "fps")
            .string("state_class", "measurement")
            .string("entity_category", "diagnostic")
            .string("icon", "mdi:speedometer-medium")
            .end_object();
        announce("sensor", "fps", json);
    }
    if (g_device.has_motion) {
        formats::JsonWriter json(buffer, sizeof(buffer));
        begin_config(json, "auto_off", "Motion auto off");
        topic(command, "auto_off/set");
        topic(state, "auto_off/state");
        json.string("command_topic", command)
            .string("state_topic", state)
            .string("icon", "mdi:motion-sensor-off")
            .string("entity_category", "config")
            .end_object();
        announce("switch", "auto_off", json);
    }
    if (g_device.has_motion) {
        formats::JsonWriter json(buffer, sizeof(buffer));
        begin_config(json, "auto_off_minutes", "Auto off without motion");
        topic(command, "auto_off_minutes/set");
        topic(state, "auto_off_minutes/state");
        json.string("command_topic", command)
            .string("state_topic", state)
            .number("min", 1)
            .number("max", leds::kMaxAutoOffMinutes)
            .string("unit_of_measurement", "min")
            .string("mode", "box")
            .string("icon", "mdi:timer-outline")
            .string("entity_category", "config")
            .end_object();
        announce("number", "auto_off_minutes", json);
    }
    if (g_device.has_motion) {
        topic(state, "motion");
        formats::JsonWriter json(buffer, sizeof(buffer));
        begin_config(json, "motion", "Motion");
        json.string("state_topic", state).string("device_class", "motion").end_object();
        announce("binary_sensor", "motion", json);
    }
    // the current animation's rotation and sound switch (each animation keeps its own)
    {
        formats::JsonWriter json(buffer, sizeof(buffer));
        begin_config(json, "rotation", "Rotation");
        topic(command, "rotation/set");
        topic(state, "rotation/state");
        json.string("command_topic", command)
            .string("state_topic", state)
            .begin_array("options")
            .string(nullptr, "0")
            .string(nullptr, "90")
            .string(nullptr, "180")
            .string(nullptr, "270")
            .end_array()
            .string("icon", "mdi:screen-rotation")
            .end_object();
        announce("select", "rotation", json);
    }
    if (!audio) {
        forget("switch", "reactive");
        forget("number", "strength");
        forget("number", "attack");
        forget("number", "release");
    }
    if (audio) {
        formats::JsonWriter json(buffer, sizeof(buffer));
        begin_config(json, "reactive", "Reacts to sound");
        topic(command, "reactive/set");
        topic(state, "reactive/state");
        json.string("command_topic", command)
            .string("state_topic", state)
            .string("icon", "mdi:music-note")
            .end_object();
        announce("switch", "reactive", json);
    }
    if (audio) {
        formats::JsonWriter json(buffer, sizeof(buffer));
        begin_config(json, "strength", "Sound strength");
        topic(command, "strength/set");
        topic(state, "strength/state");
        json.string("command_topic", command)
            .string("state_topic", state)
            .number("min", 0)
            .number("max", 255)
            .string("mode", "slider")
            .string("icon", "mdi:music-note-plus")
            .end_object();
        announce("number", "strength", json);
        announce_number(buffer, sizeof(buffer), "attack", "Sound attack", 0, effects::kMaxAttackMs, "ms",
                        "mdi:arrow-up-bold", false);
        announce_number(buffer, sizeof(buffer), "release", "Sound release", 0, effects::kMaxReleaseMs, "ms",
                        "mdi:arrow-down-bold", false);
    }
    announce_select(buffer, sizeof(buffer), "overlay", "Overlay", kOverlayModes, std::size(kOverlayModes),
                    "mdi:format-text", false);
    {
        formats::JsonWriter json(buffer, sizeof(buffer));
        begin_config(json, "overlay_text", "Overlay text");
        topic(command, "overlay_text/set");
        topic(state, "overlay_text/state");
        json.string("command_topic", command)
            .string("state_topic", state)
            .number("max", static_cast<int64_t>(leds::kOverlayTextSize - 1))
            .string("icon", "mdi:message-text")
            .end_object();
        announce("text", "overlay_text", json);
    }
    announce_select(buffer, sizeof(buffer), "power_on", "Power on", kPowerOn, std::size(kPowerOn), "mdi:power-settings",
                    true);
    {
        formats::JsonWriter json(buffer, sizeof(buffer));
        begin_config(json, "restart", "Restart");
        topic(command, "restart");
        json.string("command_topic", command)
            .string("device_class", "restart")
            .string("entity_category", "config")
            .end_object();
        announce("button", "restart", json);
    }
}

// ------------------------------------------------------------------------------- state

void publish_light(const leds::State &s, bool speed_changed)
{
    char buffer[384];
    formats::JsonWriter json(buffer, sizeof(buffer));
    const effects::Info *effect = effects::info(s.effect);
    json.begin_object()
        .string("state", s.on ? "ON" : "OFF")
        .number("brightness", s.brightness)
        .string("color_mode", "rgb")
        .begin_object("color")
        .number("r", s.color.r)
        .number("g", s.color.g)
        .number("b", s.color.b)
        .end_object()
        .string("effect", effect != nullptr ? effect->name : "")
        .end_object();
    char state[kTopicSize * 2];
    topic(state, "light/state");
    publish(state, json.c_str(), true);
    if (!speed_changed) {
        return;
    }
    topic(state, "speed/state");
    char speed[8];
    std::snprintf(speed, sizeof(speed), "%u", s.speed);
    publish(state, speed, true);
}

void publish_sensors()
{
    const leds::Status s = leds::status();
    char buffer[96];
    formats::JsonWriter json(buffer, sizeof(buffer));
    json.begin_object()
        .decimal("power", static_cast<double>(s.supply_on ? s.power_mw : 0) / 1000.0)
        .number("fps", s.fps)
        .end_object();
    char state[kTopicSize * 2];
    topic(state, "sensors");
    publish(state, json.c_str(), false);
}

bool same(const leds::State &a, const leds::State &b)
{
    return a.on == b.on && a.brightness == b.brightness && a.effect == b.effect && a.color.r == b.color.r &&
           a.color.g == b.color.g && a.color.b == b.color.b && a.speed == b.speed;
}

// the timer: what changed goes out (the esp_timer task)
void on_timer(void *)
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    const bool connected = g_status.state == State::Connected;
    const bool motion = g_motion;
    const bool motion_known = g_motion_known;
    xSemaphoreGive(g_lock);
    if (!connected) {
        return;
    }
    // an audio source came or went (a UDP input): the entities and the effect list again
    const bool audio = has_audio();
    if (!g_sent.audio_valid || g_sent.audio != audio) {
        if (g_sent.audio_valid) {
            announce_all();
            g_sent.style_valid = false;
        }
        g_sent.audio = audio;
        g_sent.audio_valid = true;
    }
    const leds::Status s = leds::status();
    if (!g_sent.valid || !same(g_sent.state, s.state)) {
        publish_light(s.state, !g_sent.valid || g_sent.state.speed != s.state.speed);
        g_sent.state = s.state;
        g_sent.valid = true;
    }
    if (!g_sent.style_valid || g_sent.style.rotation != s.style.rotation || g_sent.style.reactive != s.style.reactive ||
        g_sent.style.strength != s.style.strength || g_sent.style.attack_ms != s.style.attack_ms ||
        g_sent.style.release_ms != s.style.release_ms) {
        char state[kTopicSize * 2];
        char rotation[8];
        std::snprintf(rotation, sizeof(rotation), "%u", s.style.rotation * 90u);
        topic(state, "rotation/state");
        publish(state, rotation, true);
        if (audio) {
            topic(state, "reactive/state");
            publish(state, s.style.reactive ? "ON" : "OFF", true);
            char number[8];
            std::snprintf(number, sizeof(number), "%u", s.style.strength);
            topic(state, "strength/state");
            publish(state, number, true);
            std::snprintf(number, sizeof(number), "%u", s.style.attack_ms);
            topic(state, "attack/state");
            publish(state, number, true);
            std::snprintf(number, sizeof(number), "%u", s.style.release_ms);
            topic(state, "release/state");
            publish(state, number, true);
        }
        g_sent.style = s.style;
        g_sent.style_valid = true;
    }
    const leds::Overlay o = leds::overlay();
    if (!g_sent.overlay_valid || g_sent.overlay.mode != o.mode || std::strcmp(g_sent.overlay.text, o.text) != 0) {
        char state[kTopicSize * 2];
        topic(state, "overlay/state");
        publish(state, kOverlayModes[static_cast<size_t>(o.mode) % std::size(kOverlayModes)], true);
        topic(state, "overlay_text/state");
        publish(state, o.text, true);
        g_sent.overlay = o;
        g_sent.overlay_valid = true;
    }
    const leds::PowerOn power_on = leds::config().power_on;
    if (!g_sent.power_on_valid || g_sent.power_on != power_on) {
        char state[kTopicSize * 2];
        topic(state, "power_on/state");
        publish(state, kPowerOn[static_cast<size_t>(power_on) % std::size(kPowerOn)], true);
        g_sent.power_on = power_on;
        g_sent.power_on_valid = true;
    }
    const int64_t now = esp_timer_get_time();
    if (now - g_sent.sensors_at >= kSensorUs) {
        publish_sensors();
        g_sent.sensors_at = now;
    }
    if (g_device.has_motion) {
        const leds::AutoOff a = leds::auto_off();
        if (!g_sent.auto_off_valid || g_sent.auto_off.enabled != a.enabled || g_sent.auto_off.minutes != a.minutes) {
            char state[kTopicSize * 2];
            topic(state, "auto_off/state");
            publish(state, a.enabled ? "ON" : "OFF", true);
            char minutes[8];
            std::snprintf(minutes, sizeof(minutes), "%u", a.minutes);
            topic(state, "auto_off_minutes/state");
            publish(state, minutes, true);
            g_sent.auto_off = a;
            g_sent.auto_off_valid = true;
        }
    }
    if (g_device.has_motion && motion_known && (!g_sent.motion_valid || g_sent.motion != motion)) {
        char state[kTopicSize * 2];
        topic(state, "motion");
        publish(state, motion ? "ON" : "OFF", true);
        g_sent.motion = motion;
        g_sent.motion_valid = true;
    }
}

// ------------------------------------------------------------------------------- commands

void light_command(const char *data, size_t length)
{
    cJSON *root = cJSON_ParseWithLength(data, length);
    if (root == nullptr) {
        ESP_LOGW(kTag, "light: not JSON");
        return;
    }
    // the order of a person: animation and color first, then brightness, then on
    if (const cJSON *effect = cJSON_GetObjectItem(root, "effect"); cJSON_IsString(effect)) {
        for (size_t i = 0; i < effects::count(); i++) {
            if (std::strcmp(effects::info(i)->name, effect->valuestring) == 0 ||
                std::strcmp(effects::info(i)->slug, effect->valuestring) == 0) {
                leds::set_effect(i);
            }
        }
    }
    if (const cJSON *color = cJSON_GetObjectItem(root, "color"); cJSON_IsObject(color)) {
        const cJSON *r = cJSON_GetObjectItem(color, "r");
        const cJSON *g = cJSON_GetObjectItem(color, "g");
        const cJSON *b = cJSON_GetObjectItem(color, "b");
        if (cJSON_IsNumber(r) && cJSON_IsNumber(g) && cJSON_IsNumber(b)) {
            leds::set_color({static_cast<uint8_t>(r->valueint), static_cast<uint8_t>(g->valueint),
                             static_cast<uint8_t>(b->valueint)});
        }
    }
    if (const cJSON *brightness = cJSON_GetObjectItem(root, "brightness"); cJSON_IsNumber(brightness)) {
        const int value = brightness->valueint;
        leds::set_brightness(static_cast<uint8_t>(value < 1 ? 1 : value > 255 ? 255 : value));
    }
    if (const cJSON *state = cJSON_GetObjectItem(root, "state"); cJSON_IsString(state)) {
        leds::set_on(std::strcmp(state->valuestring, "ON") == 0);
    }
    cJSON_Delete(root);
}

void handle_data(const esp_mqtt_event_t &event)
{
    if (event.current_data_offset != 0 || event.data_len != event.total_data_len) {
        return; // the commands are small: a split message is none of them
    }
    char name[kTopicSize * 2];
    std::snprintf(name, sizeof(name), "%.*s", event.topic_len, event.topic);
    char expected[kTopicSize * 2];
    topic(expected, "light/set");
    if (std::strcmp(name, expected) == 0) {
        light_command(event.data, static_cast<size_t>(event.data_len));
        return;
    }
    topic(expected, "speed/set");
    if (std::strcmp(name, expected) == 0) {
        char text[8];
        std::snprintf(text, sizeof(text), "%.*s", event.data_len, event.data);
        const int value = std::atoi(text);
        leds::set_speed(static_cast<uint8_t>(value < 0 ? 0 : value > 255 ? 255 : value));
        return;
    }
    topic(expected, "auto_off/set");
    if (std::strcmp(name, expected) == 0) {
        leds::AutoOff a = leds::auto_off();
        a.enabled = event.data_len == 2 && std::strncmp(event.data, "ON", 2) == 0;
        leds::set_auto_off(a);
        return;
    }
    topic(expected, "auto_off_minutes/set");
    if (std::strcmp(name, expected) == 0) {
        char text[8];
        std::snprintf(text, sizeof(text), "%.*s", event.data_len, event.data);
        leds::AutoOff a = leds::auto_off();
        a.minutes = static_cast<uint16_t>(std::atoi(text));
        leds::set_auto_off(a); // clamped there
        return;
    }
    topic(expected, "rotation/set");
    if (std::strcmp(name, expected) == 0) {
        char text[8];
        std::snprintf(text, sizeof(text), "%.*s", event.data_len, event.data);
        const int degrees = std::atoi(text);
        const size_t effect = leds::status().state.effect;
        leds::Style style = leds::styles().style[effect < leds::kMaxEffects ? effect : 0];
        if (degrees % 90 == 0 && degrees >= 0 && degrees < 360) {
            style.rotation = static_cast<uint8_t>(degrees / 90);
            leds::set_style(effect, style);
        }
        return;
    }
    topic(expected, "reactive/set");
    if (std::strcmp(name, expected) == 0) {
        const size_t effect = leds::status().state.effect;
        leds::Style style = leds::styles().style[effect < leds::kMaxEffects ? effect : 0];
        style.reactive = event.data_len == 2 && std::strncmp(event.data, "ON", 2) == 0;
        leds::set_style(effect, style);
        return;
    }
    topic(expected, "strength/set");
    if (std::strcmp(name, expected) == 0) {
        char text[8];
        std::snprintf(text, sizeof(text), "%.*s", event.data_len, event.data);
        const int value = std::atoi(text);
        const size_t effect = leds::status().state.effect;
        leds::Style style = leds::styles().style[effect < leds::kMaxEffects ? effect : 0];
        style.strength = static_cast<uint8_t>(value < 0 ? 0 : value > 255 ? 255 : value);
        leds::set_style(effect, style);
        return;
    }
    topic(expected, "attack/set");
    const bool attack = std::strcmp(name, expected) == 0;
    topic(expected, "release/set");
    if (attack || std::strcmp(name, expected) == 0) {
        char text[8];
        std::snprintf(text, sizeof(text), "%.*s", event.data_len, event.data);
        const int value = std::atoi(text);
        const size_t effect = leds::status().state.effect;
        leds::Style style = leds::styles().style[effect < leds::kMaxEffects ? effect : 0];
        // the largest values are clamped by set_style
        const auto ms = static_cast<uint16_t>(value < 0 ? 0 : value > 65535 ? 65535 : value);
        if (attack) {
            style.attack_ms = ms;
        } else {
            style.release_ms = ms;
        }
        leds::set_style(effect, style);
        return;
    }
    topic(expected, "overlay/set");
    if (std::strcmp(name, expected) == 0) {
        for (size_t i = 0; i < std::size(kOverlayModes); i++) {
            if (std::strlen(kOverlayModes[i]) == static_cast<size_t>(event.data_len) &&
                std::strncmp(event.data, kOverlayModes[i], event.data_len) == 0) {
                leds::Overlay o = leds::overlay();
                o.mode = static_cast<leds::OverlayMode>(i);
                leds::set_overlay(o);
            }
        }
        return;
    }
    topic(expected, "overlay_text/set");
    if (std::strcmp(name, expected) == 0) {
        leds::Overlay o = leds::overlay();
        std::snprintf(o.text, sizeof(o.text), "%.*s", event.data_len, event.data);
        leds::set_overlay(o);
        return;
    }
    topic(expected, "power_on/set");
    if (std::strcmp(name, expected) == 0) {
        for (size_t i = 0; i < std::size(kPowerOn); i++) {
            if (std::strlen(kPowerOn[i]) == static_cast<size_t>(event.data_len) &&
                std::strncmp(event.data, kPowerOn[i], event.data_len) == 0) {
                leds::Config c = leds::config();
                c.power_on = static_cast<leds::PowerOn>(i);
                char error[64];
                leds::set_config(c, error, sizeof(error));
            }
        }
        return;
    }
    topic(expected, "restart");
    if (std::strcmp(name, expected) == 0) {
        ESP_LOGI(kTag, "restart from Home Assistant");
        esp_restart();
    }
}

void on_event(void *, esp_event_base_t, int32_t id, void *data)
{
    const auto &event = *static_cast<esp_mqtt_event_t *>(data);
    switch (static_cast<esp_mqtt_event_id_t>(id)) {
    case MQTT_EVENT_CONNECTED: {
        ESP_LOGI(kTag, "connected to %s", g_settings.uri);
        xSemaphoreTake(g_lock, portMAX_DELAY);
        g_connected_at = esp_timer_get_time();
        g_sent = {};
        xSemaphoreGive(g_lock);
        set_status(State::Connected, "");
        char name[kTopicSize * 2];
        topic(name, "availability");
        publish(name, "online", true);
        announce_all();
        for (const char *suffix :
             {"light/set", "speed/set", "restart", "auto_off/set", "auto_off_minutes/set", "rotation/set", "reactive/set",
              "strength/set", "attack/set", "release/set", "overlay/set", "overlay_text/set", "power_on/set"}) {
            topic(name, suffix);
            esp_mqtt_client_subscribe(event.client, name, 1);
        }
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        set_status(State::Disconnected, nullptr);
        break;
    case MQTT_EVENT_ERROR:
        if (event.error_handle != nullptr &&
            event.error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            set_status(State::Disconnected, event.error_handle->connect_return_code ==
                                                    MQTT_CONNECTION_REFUSE_BAD_USERNAME ||
                                                    event.error_handle->connect_return_code ==
                                                        MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED
                                                ? "the broker refused the login"
                                                : "the broker refused the connection");
        } else {
            set_status(State::Disconnected, "the broker does not answer");
        }
        break;
    case MQTT_EVENT_DATA:
        handle_data(event);
        break;
    default:
        break;
    }
}

// ------------------------------------------------------------------------------- the client

void stop_client()
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    esp_mqtt_client_handle_t client = g_client;
    g_client = nullptr;
    xSemaphoreGive(g_lock);
    if (client != nullptr) {
        // waits for the client's task, which may still publish: not under the lock
        esp_mqtt_client_stop(client);
        esp_mqtt_client_destroy(client);
    }
    set_status(State::Off, "");
}

void start_client()
{
    stop_client();
    if (g_settings.base[0] != 0) {
        std::snprintf(g_base, sizeof(g_base), "%s", g_settings.base);
    } else {
        std::snprintf(g_base, sizeof(g_base), "pixeled/%s", g_device.name);
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    std::snprintf(g_status.base, sizeof(g_status.base), "%s", g_base);
    xSemaphoreGive(g_lock);
    if (!g_settings.enabled || g_settings.uri[0] == 0) {
        return;
    }
    static char will[kTopicSize * 2];
    topic(will, "availability");
    esp_mqtt_client_config_t config{};
    config.broker.address.uri = g_settings.uri;
    config.credentials.client_id = g_device.name;
    config.credentials.username = g_settings.username[0] != 0 ? g_settings.username : nullptr;
    config.credentials.authentication.password = g_password[0] != 0 ? g_password : nullptr;
    config.session.last_will.topic = will;
    config.session.last_will.msg = "offline";
    config.session.last_will.qos = 1;
    config.session.last_will.retain = 1;
    config.session.keepalive = 30;
    config.network.reconnect_timeout_ms = 10000;
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&config);
    if (client == nullptr) {
        set_status(State::Disconnected, "the client could not be created (the address?)");
        return;
    }
    esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, on_event, nullptr);
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_client = client;
    xSemaphoreGive(g_lock);
    if (esp_mqtt_client_start(client) != ESP_OK) {
        set_status(State::Disconnected, "the client did not start");
        return;
    }
    set_status(State::Connecting, "");
}

template <class T>
void load(T &out)
{
    platform::Settings settings(kNamespace, platform::Settings::Mode::Read);
    T stored{};
    size_t size = sizeof(stored);
    if (settings.ok() && settings.get_blob(kKeySettings, &stored, size) == ESP_OK && size == sizeof(stored) &&
        stored.version == T{}.version) {
        out = stored;
    }
    if (settings.ok()) {
        settings.get_string(kKeyPassword, g_password, sizeof(g_password));
    }
}

} // namespace

const char *state_name(State state)
{
    switch (state) {
    case State::Off:
        return "off";
    case State::Connecting:
        return "connecting";
    case State::Connected:
        return "connected";
    case State::Disconnected:
        return "disconnected";
    }
    return "?";
}

esp_err_t start(const Device &device)
{
    ESP_RETURN_ON_FALSE(g_lock == nullptr, ESP_ERR_INVALID_STATE, kTag, "already started");
    g_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(g_lock != nullptr, ESP_ERR_NO_MEM, kTag, "lock");
    g_device = device;
    size_t i = 0;
    for (; device.name[i] != 0 && i + 1 < sizeof(g_node); i++) {
        g_node[i] = device.name[i] == '-' ? '_' : device.name[i];
    }
    g_node[i] = 0;
    load(g_settings);
    esp_timer_create_args_t timer{};
    timer.callback = on_timer;
    timer.name = "mqtt";
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer, &g_timer), kTag, "timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(g_timer, kCheckUs), kTag, "timer");
    start_client();
    return ESP_OK;
}

bool set_settings(const Settings &in, const char *password, char *error, size_t error_size)
{
    if (g_lock == nullptr) {
        std::snprintf(error, error_size, "MQTT does not run");
        return false;
    }
    const bool scheme = std::strncmp(in.uri, "mqtt://", 7) == 0 || std::strncmp(in.uri, "mqtts://", 8) == 0 ||
                        std::strncmp(in.uri, "ws://", 5) == 0 || std::strncmp(in.uri, "wss://", 6) == 0;
    const char *problem = in.enabled && in.uri[0] == 0         ? "the broker's address is missing"
                          : in.uri[0] != 0 && !scheme          ? "address: mqtt://host:1883 or mqtts://host:8883"
                          : in.prefix[0] == 0                  ? "the discovery prefix is missing"
                          : std::strchr(in.base, '#') != nullptr || std::strchr(in.base, '+') != nullptr
                              ? "the base topic may not have # or +"
                              : nullptr;
    if (problem != nullptr) {
        std::snprintf(error, error_size, "%s", problem);
        return false;
    }
    Settings stored = in;
    stored.version = Settings{}.version;
    {
        platform::Settings nvs(kNamespace, platform::Settings::Mode::ReadWrite);
        bool ok = nvs.ok() && nvs.set_blob(kKeySettings, &stored, sizeof(stored)) == ESP_OK;
        if (ok && password != nullptr) {
            ok = password[0] == 0 ? (nvs.erase(kKeyPassword), true) : nvs.set_string(kKeyPassword, password) == ESP_OK;
        }
        if (!ok || nvs.commit() != ESP_OK) {
            std::snprintf(error, error_size, "the settings could not be stored");
            return false;
        }
    }
    // the client is created and destroyed by the caller's task (the web server's), never by its own
    g_settings = stored;
    if (password != nullptr) {
        std::snprintf(g_password, sizeof(g_password), "%s", password);
    }
    start_client();
    return true;
}

Settings settings()
{
    return g_settings;
}

bool password_set()
{
    return g_password[0] != 0;
}

Status status()
{
    if (g_lock == nullptr) {
        return {};
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    Status out = g_status;
    out.connected_s = out.state == State::Connected
                          ? static_cast<uint32_t>((esp_timer_get_time() - g_connected_at) / 1000000)
                          : 0;
    xSemaphoreGive(g_lock);
    return out;
}

void publish_motion(bool detected)
{
    if (g_lock == nullptr) {
        return;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_motion = detected;
    g_motion_known = true;
    xSemaphoreGive(g_lock);
}

} // namespace px::hass
