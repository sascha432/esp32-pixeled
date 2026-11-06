// The scripts in the web API (docs/ui/web/web-app.md section 4, docs/glow.md), login required; only
// with CONFIG_PX_SCRIPTS:
//
//   GET  /api/scripts                     {"slots", "stack", "scripts": [a script], "run": a script
//                                         or null}; a script: {"slot", "slug", "name", "ok",
//                                         "error": {"line", "column", "text"} or null, "size",
//                                         "program", "reactive", "color", "palette", "params":
//                                         [{"name", "label", "min", "max", "step", "slider", "value"}]
//                                         for the script shown, [] for the others (their programs are
//                                         not read for a list)}
//   GET  /api/scripts/source?slug=<slug>  the source, text/plain; 404 when there is none
//   POST /api/scripts/save?slug=<slug>    the source as the body: saved and compiled, {"ok", "error"}
//                                         (ok false: saved, the program before runs on); 400 for a
//                                         bad slug, 507 without a free slot
//   POST /api/scripts/run                 the source as the body: compiled and shown, not saved;
//                                         {"ok", "error"}
//   POST /api/scripts/delete?slug=<slug>  {"ok": true}; 404 when there is none
//
// The params' sliders are set with POST /api/leds (p0..p7, api_leds.cpp).

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "px/leds/leds.hpp"
#include "px/memory/large.hpp"
#include "px/scripts/scripts.hpp"
#include "px/webserver/http.hpp"

#include "api_scripts.hpp"

namespace px::leds_api {

namespace {

namespace glow = effects::glow;

using webserver::detail::JsonWriter;
using webserver::detail::query_value;
using webserver::detail::read_large_body;
using webserver::detail::require_login;
using webserver::detail::route;
using webserver::detail::send_error;
using webserver::detail::send_json;

double real(int32_t fix)
{
    return static_cast<double>(fix) / 65536.0;
}

void write_error(JsonWriter &json, const glow::Error &error)
{
    if (error.text[0] == '\0') {
        json.null("error");
        return;
    }
    json.begin_object("error")
        .number("line", error.line)
        .number("column", error.column)
        .string("text", error.text)
        .end_object();
}

struct ListContext {
    JsonWriter *json;
    int shown;
    bool run;
};

void write_script(JsonWriter &json, const char *key, size_t slot, const scripts::Script &s, int shown)
{
    json.begin_object(key)
        .number("slot", static_cast<int64_t>(slot))
        .string("slug", s.slug)
        .string("name", s.name)
        .boolean("ok", s.ok);
    write_error(json, s.error);
    json.number("size", s.source_size)
        .number("program", s.program_size)
        .string("reactive", s.reactive == effects::Reactive::Never      ? "never"
                            : s.reactive == effects::Reactive::Optional ? "optional"
                                                                         : "required")
        .boolean("color", s.uses_color)
        .boolean("palette", s.uses_palette)
        .begin_array("params");
    if (static_cast<int>(slot) == shown && s.program_size != 0) {
        write_script_params(json, slot, true);
    }
    json.end_array().end_object();
}

// GET /api/scripts, pushed on the live socket as well
void write_scripts(JsonWriter &json)
{
    json.begin_object()
        .number("slots", static_cast<int64_t>(leds::kScriptSlots))
        .number("stack", scripts::compiler_stack_peak())
        .begin_array("scripts");
    ListContext c{&json, leds::script_slot(leds::status().state.effect), false};
    scripts::each(
        [](size_t slot, const scripts::Script &s, void *context) {
            auto &c = *static_cast<ListContext *>(context);
            if (slot < leds::kRunSlot) {
                write_script(*c.json, nullptr, slot, s, c.shown);
            }
        },
        &c);
    json.end_array();
    // the run after the list
    scripts::each(
        [](size_t slot, const scripts::Script &s, void *context) {
            auto &c = *static_cast<ListContext *>(context);
            if (slot == leds::kRunSlot) {
                write_script(*c.json, "run", slot, s, c.shown);
                c.run = true;
            }
        },
        &c);
    if (!c.run) {
        json.null("run");
    }
    json.end_object();
}

esp_err_t get_scripts(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    return webserver::detail::send_state(req, write_scripts);
}

esp_err_t get_source(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    char slug[scripts::kSlugSize] = {};
    if (!query_value(req, "slug", slug, sizeof(slug)) || !scripts::valid_slug(slug)) {
        return send_error(req, "400 Bad Request", "slug: the script's file name");
    }
    auto *text = static_cast<char *>(memory::allocate_large(glow::kMaxSource + 1));
    if (text == nullptr) {
        return send_error(req, "500 Internal Server Error", "no memory");
    }
    size_t length = 0;
    if (scripts::source(slug, text, glow::kMaxSource + 1, length) != ESP_OK) {
        memory::release_large(text);
        return send_error(req, "404 Not Found", "no such script");
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_send(req, text, static_cast<ssize_t>(length));
    memory::release_large(text);
    return err;
}

esp_err_t send_result(httpd_req_t *req, const glow::Error &error)
{
    char buffer[192];
    JsonWriter json(buffer, sizeof(buffer));
    json.begin_object().boolean("ok", error.text[0] == '\0');
    write_error(json, error);
    json.end_object();
    return send_json(req, json);
}

// the source of a save or a run; nullptr with the answer sent
char *read_source(httpd_req_t *req, size_t &length)
{
    auto body = read_large_body(req, glow::kMaxSource, length);
    if (body) {
        return *body;
    }
    if (body.error() == ESP_ERR_INVALID_SIZE) {
        send_error(req, "400 Bad Request", "the script is empty or longer than 8192 bytes");
    } else {
        send_error(req, "500 Internal Server Error", "the script could not be read");
    }
    return nullptr;
}

esp_err_t post_save(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    char slug[scripts::kSlugSize] = {};
    if (!query_value(req, "slug", slug, sizeof(slug)) || !scripts::valid_slug(slug)) {
        return send_error(req, "400 Bad Request", "slug: 1 to 23 of a-z, 0-9 and -");
    }
    size_t length = 0;
    char *text = read_source(req, length);
    if (text == nullptr) {
        return ESP_OK;
    }
    glow::Error error;
    const esp_err_t err = scripts::save(slug, {text, length}, error);
    memory::release_large(text);
    if (err == ESP_ERR_INVALID_ARG) {
        return send_error(req, "400 Bad Request", error.text);
    }
    if (err == ESP_ERR_NO_MEM) {
        return send_error(req, "507 Insufficient Storage", error.text);
    }
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", error.text);
    }
    return send_result(req, error);
}

esp_err_t post_run(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    size_t length = 0;
    char *text = read_source(req, length);
    if (text == nullptr) {
        return ESP_OK;
    }
    glow::Error error;
    const esp_err_t err = scripts::run({text, length}, error);
    memory::release_large(text);
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", error.text);
    }
    return send_result(req, error);
}

esp_err_t post_delete(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    char slug[scripts::kSlugSize] = {};
    if (!query_value(req, "slug", slug, sizeof(slug)) || scripts::remove(slug) != ESP_OK) {
        return send_error(req, "404 Not Found", "no such script");
    }
    char buffer[16];
    JsonWriter json(buffer, sizeof(buffer));
    json.begin_object().boolean("ok", true).end_object();
    return send_json(req, json);
}

} // namespace

void write_script_params(JsonWriter &json, size_t slot, bool full)
{
    auto *info = static_cast<glow::Info *>(memory::allocate_large(sizeof(glow::Info)));
    char slug[leds::kScriptSlugSize];
    // a file read stops the code in flash on both cores for its time: not while the LEDs render
    if (info == nullptr ||
        (!leds::running_info(leds::script_effect(slot), slug, *info) && !scripts::params(slot, *info))) {
        memory::release_large(info);
        return;
    }
    const leds::ScriptStyle style = leds::script_style(slot);
    for (size_t k = 0; k < info->param_count; k++) {
        const glow::Param &p = info->params[k];
        const uint8_t slider = style.sliders[k];
        json.begin_object();
        if (full) {
            json.string("name", p.name)
                .decimal("min", real(p.min))
                .decimal("max", real(p.max))
                .decimal("step", real(p.step));
        }
        json.string("label", p.label).number("slider", slider).decimal("value", real(glow::param_value(p, slider)));
        json.end_object();
    }
    memory::release_large(info);
}

void register_script_routes(void *server)
{
    webserver::detail::add_live_topic("/api/scripts", 2000, write_scripts);
    const httpd_uri_t routes[] = {
        route("/api/scripts", HTTP_GET, get_scripts),
        route("/api/scripts/source", HTTP_GET, get_source),
        route("/api/scripts/save", HTTP_POST, post_save),
        route("/api/scripts/run", HTTP_POST, post_run),
        route("/api/scripts/delete", HTTP_POST, post_delete),
    };
    for (const auto &r : routes) {
        httpd_register_uri_handler(static_cast<httpd_handle_t>(server), &r);
    }
}

} // namespace px::leds_api
