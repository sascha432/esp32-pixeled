// The presets in the web API (docs/ui/web/web-app.md section 4), login required:
//
//   GET  /api/presets          {"presets": [{"name", "effect" (slug), "color", "speed", "brightness",
//                              "palette" (slug)}], "current": the index the light matches, -1: none,
//                              "max"}
//   POST /api/presets/save     name: the light as it is now under this name (replaces one of the
//                              same name); answers like GET, 400 with the problem
//   POST /api/presets/apply    index: shows that preset; answers like GET
//   POST /api/presets/delete   index; answers like GET

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "px/effects/effects.hpp"
#include "px/presets/presets.hpp"
#include "px/webserver/http.hpp"

#include "api_presets.hpp"

namespace px::leds_api {

namespace {

using webserver::detail::form_value;
using webserver::detail::JsonWriter;
using webserver::detail::read_body;
using webserver::detail::require_login;
using webserver::detail::route;
using webserver::detail::send_error;
using webserver::detail::send_json;

esp_err_t send_presets(httpd_req_t *req)
{
    static char buffer[3072]; // the server has one task
    JsonWriter json(buffer, sizeof(buffer));
    const size_t count = presets::count();
    const size_t current = presets::current();
    json.begin_object().begin_array("presets");
    for (size_t i = 0; i < count; i++) {
        presets::Preset p{};
        if (!presets::get(i, p)) {
            break;
        }
        char color[8];
        std::snprintf(color, sizeof(color), "#%02x%02x%02x", p.color.r, p.color.g, p.color.b);
        const effects::Info *effect = effects::info(p.effect);
        const effects::PaletteInfo *palette = effects::palette_info(p.style.palette);
        json.begin_object()
            .string("name", p.name)
            .string("effect", effect != nullptr ? effect->slug : "")
            .string("color", color)
            .number("speed", p.speed)
            .number("brightness", p.brightness)
            .string("palette", palette != nullptr ? palette->slug : "none")
            .end_object();
    }
    json.end_array()
        .number("current", current < count ? static_cast<int64_t>(current) : -1)
        .number("max", static_cast<int64_t>(presets::kMaxPresets))
        .end_object();
    return send_json(req, json);
}

esp_err_t get_presets(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    return send_presets(req);
}

// the index of the form, false (400 sent) when it is not one of a preset
bool form_index(httpd_req_t *req, const char *body, size_t &out)
{
    char text[8] = {};
    char *end = nullptr;
    if (!form_value(body, "index", text, sizeof(text)) || text[0] == 0 ||
        (out = std::strtoul(text, &end, 10), *end != 0) || out >= presets::count()) {
        send_error(req, "400 Bad Request", "index: the number of a preset, from 0");
        return false;
    }
    return true;
}

esp_err_t post_save(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    char body[160];
    if (!read_body(req, body, sizeof(body))) {
        return send_error(req, "400 Bad Request", "the request is too large");
    }
    char name[presets::kNameSize + 1] = {};
    if (!form_value(body, "name", name, sizeof(name))) {
        return send_error(req, "400 Bad Request", "name: 1 to 31 bytes");
    }
    char error[64] = {};
    if (!presets::save_current(name, error, sizeof(error))) {
        return send_error(req, "400 Bad Request", error);
    }
    return send_presets(req);
}

esp_err_t post_apply(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    char body[64];
    size_t index = 0;
    if (!read_body(req, body, sizeof(body))) {
        return send_error(req, "400 Bad Request", "the request is too large");
    }
    if (!form_index(req, body, index)) {
        return ESP_OK;
    }
    presets::apply(index);
    // the render task takes it with its next loop
    vTaskDelay(pdMS_TO_TICKS(50));
    return send_presets(req);
}

esp_err_t post_delete(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    char body[64];
    size_t index = 0;
    if (!read_body(req, body, sizeof(body))) {
        return send_error(req, "400 Bad Request", "the request is too large");
    }
    if (!form_index(req, body, index)) {
        return ESP_OK;
    }
    char error[64] = {};
    if (!presets::remove(index, error, sizeof(error))) {
        return send_error(req, "400 Bad Request", error);
    }
    return send_presets(req);
}

} // namespace

void register_preset_routes(void *server)
{
    const httpd_uri_t routes[] = {
        route("/api/presets", HTTP_GET, get_presets),
        route("/api/presets/save", HTTP_POST, post_save),
        route("/api/presets/apply", HTTP_POST, post_apply),
        route("/api/presets/delete", HTTP_POST, post_delete),
    };
    for (const auto &r : routes) {
        httpd_register_uri_handler(static_cast<httpd_handle_t>(server), &r);
    }
}

} // namespace px::leds_api
