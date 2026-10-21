// MQTT in the web API (docs/ui/web/web-app.md section 4), login required:
//
//   GET  /api/mqtt    {"enabled", "uri", "username", "password_set", "prefix", "base", "state",
//                     "error", "connected_s", "topics"}; the password never
//   POST /api/mqtt    enabled=1|0, uri, username, password (missing or empty keeps the stored one),
//                     forget_password=1, prefix, base; answers like GET, 400 with the problem
//
// Only in the main firmware: main registers the routes with the LED API.

#include <cstdio>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "px/hass/hass.hpp"
#include "px/webserver/http.hpp"

namespace px::hass {

namespace {

using webserver::detail::form_has;
using webserver::detail::form_value;
using webserver::detail::JsonWriter;
using webserver::detail::read_body;
using webserver::detail::require_login;
using webserver::detail::send_error;
using webserver::detail::send_json;

esp_err_t send_settings(httpd_req_t *req)
{
    const Settings s = settings();
    const Status st = status();
    char buffer[768];
    JsonWriter json(buffer, sizeof(buffer));
    json.begin_object()
        .boolean("enabled", s.enabled)
        .string("uri", s.uri)
        .string("username", s.username)
        .boolean("password_set", password_set())
        .string("prefix", s.prefix)
        .string("base", s.base)
        .string("state", state_name(st.state))
        .string("error", st.error)
        .number("connected_s", st.connected_s)
        .string("topics", st.base)
        .end_object();
    return send_json(req, json);
}

esp_err_t get_mqtt(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    return send_settings(req);
}

esp_err_t post_mqtt(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    static char body[512];
    if (!read_body(req, body, sizeof(body))) {
        return send_error(req, "400 Bad Request", "the request is too large");
    }
    Settings s = settings();
    char text[8] = {};
    if (form_value(body, "enabled", text, sizeof(text))) {
        s.enabled = std::strcmp(text, "1") == 0;
    }
    const struct {
        const char *key;
        char *out;
        size_t size;
    } fields[] = {
        {"uri", s.uri, sizeof(s.uri)},
        {"username", s.username, sizeof(s.username)},
        {"prefix", s.prefix, sizeof(s.prefix)},
        {"base", s.base, sizeof(s.base)},
    };
    for (const auto &f : fields) {
        if (form_has(body, f.key) && !form_value(body, f.key, f.out, f.size)) {
            char message[48];
            std::snprintf(message, sizeof(message), "%s: at most %u characters", f.key,
                          static_cast<unsigned>(f.size - 1));
            return send_error(req, "400 Bad Request", message);
        }
    }
    char password[kPasswordSize] = {};
    const char *new_password = nullptr;
    if (form_value(body, "forget_password", text, sizeof(text)) && std::strcmp(text, "1") == 0) {
        new_password = "";
    } else if (form_has(body, "password")) {
        if (!form_value(body, "password", password, sizeof(password))) {
            return send_error(req, "400 Bad Request", "password: at most 64 characters");
        }
        new_password = password[0] != 0 ? password : nullptr;
    }
    char error[96] = {};
    if (!set_settings(s, new_password, error, sizeof(error))) {
        return send_error(req, "400 Bad Request", error);
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    return send_settings(req);
}

} // namespace

void register_routes(void *server)
{
    const httpd_uri_t routes[] = {
        {.uri = "/api/mqtt", .method = HTTP_GET, .handler = get_mqtt, .user_ctx = nullptr},
        {.uri = "/api/mqtt", .method = HTTP_POST, .handler = post_mqtt, .user_ctx = nullptr},
    };
    for (const auto &route : routes) {
        httpd_register_uri_handler(static_cast<httpd_handle_t>(server), &route);
    }
}

} // namespace px::hass
