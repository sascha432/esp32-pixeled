// The buttons in the web API (docs/ui/web/web-app.md section 4), login required:
//
//   GET  /api/input   {"running", "buttons": [{"button": 1..4, "gpio", "pressed", "hotspot" (held
//                     at boot it opens the hotspot), "click", "double", "hold"}] for the buttons the
//                     board has, "encoder": {"a", "b", "button" (the
//                     pins, -1: none), "position", "knob": "brightness"|"animations", "pressed",
//                     "click", "double", "hold"} or null without one, "actions": [{"slug", "name"}]}
//   POST /api/input   button=1..4|encoder and any of click, double, hold (action slugs); answers like
//                     GET, 400 with the problem
//
// Only in the main firmware: main registers the routes.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include "px/input/input.hpp"
#include "px/webserver/http.hpp"

namespace px::input {

namespace {

using webserver::detail::form_value;
using webserver::detail::JsonWriter;
using webserver::detail::read_body;
using webserver::detail::require_login;
using webserver::detail::route;
using webserver::detail::send_error;
using webserver::detail::send_json;

void write_actions(JsonWriter &json, const Actions &a)
{
    json.string("click", action_slug(a.click))
        .string("double", action_slug(a.double_click))
        .string("hold", action_slug(a.hold));
}

esp_err_t send_state(httpd_req_t *req)
{
    const Status s = status();
    const Settings c = settings();
    static char buffer[2048]; // the server has one task
    JsonWriter json(buffer, sizeof(buffer));
    json.begin_object().boolean("running", s.running).begin_array("buttons");
    for (size_t i = 0; i < kMaxButtons; i++) {
        if (s.button_gpio[i] < 0) {
            continue;
        }
        json.begin_object()
            .number("button", static_cast<int64_t>(i + 1))
            .number("gpio", s.button_gpio[i])
            .boolean("pressed", s.pressed[i])
            .boolean("hotspot", s.hotspot_button == i);
        write_actions(json, c.button[i]);
        json.end_object();
    }
    json.end_array();
    if (s.encoder_a >= 0 || s.encoder_button >= 0) {
        json.begin_object("encoder")
            .number("a", s.encoder_a)
            .number("b", s.encoder_b)
            .number("button", s.encoder_button)
            .number("position", s.encoder_position)
            .string("knob", s.knob_on_animations ? "animations" : "brightness")
            .boolean("pressed", s.pressed[kEncoderButton]);
        write_actions(json, c.button[kEncoderButton]);
        json.end_object();
    } else {
        json.null("encoder");
    }
    json.begin_array("actions");
    for (size_t i = 0; i < kActionCount; i++) {
        json.begin_object()
            .string("slug", action_slug(static_cast<Action>(i)))
            .string("name", action_name(static_cast<Action>(i)))
            .end_object();
    }
    json.end_array().end_object();
    return send_json(req, json);
}

esp_err_t get_input(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    return send_state(req);
}

esp_err_t post_input(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    char body[256];
    if (!read_body(req, body, sizeof(body))) {
        return send_error(req, "400 Bad Request", "the request is too large");
    }
    char text[24] = {};
    if (!form_value(body, "button", text, sizeof(text))) {
        return send_error(req, "400 Bad Request", "button is needed");
    }
    size_t index = kMaxButtons + 1;
    if (std::strcmp(text, "encoder") == 0) {
        index = kEncoderButton;
    } else if (text[0] >= '1' && text[0] < static_cast<char>('1' + kMaxButtons) && text[1] == 0) {
        index = static_cast<size_t>(text[0] - '1');
    }
    if (index > kMaxButtons) {
        return send_error(req, "400 Bad Request", "button: 1 to 4 or encoder");
    }
    Settings c = settings();
    Actions &a = c.button[index];
    struct Field {
        const char *key;
        Action *action;
    };
    for (const Field &f : {Field{"click", &a.click}, Field{"double", &a.double_click}, Field{"hold", &a.hold}}) {
        if (form_value(body, f.key, text, sizeof(text)) && !find_action(text, *f.action)) {
            char message[48];
            std::snprintf(message, sizeof(message), "%s: not an action", f.key);
            return send_error(req, "400 Bad Request", message);
        }
    }
    char error[64] = {};
    if (!set_settings(c, error, sizeof(error))) {
        return send_error(req, "400 Bad Request", error);
    }
    return send_state(req);
}

} // namespace

void register_routes(void *server)
{
    const httpd_uri_t routes[] = {
        route("/api/input", HTTP_GET, get_input),
        route("/api/input", HTTP_POST, post_input),
    };
    for (const auto &r : routes) {
        httpd_register_uri_handler(static_cast<httpd_handle_t>(server), &r);
    }
}

} // namespace px::input
