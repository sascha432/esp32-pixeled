// The audio input in the web API (docs/ui/web/web-app.md section 4), login required:
//
//   GET  /api/audio   {"running" (the microphone), "microphone", "source": ""|"microphone"|"warls"|
//                     "wled", "packets", "slot", "blocks", "active", "loudness", "beat",
//                     "loudest_db", "reference_db", "bands": [32 x 0..255], "settings": {"gain_db",
//                     "agc", "silence_db", "range_db", "beat_ratio_tenths"}, "inputs": {"warls",
//                     "warls_port", "wled_sync"}}
//   POST /api/audio   any of gain_db, agc=1|0, silence_db, range_db, beat_ratio_tenths, warls=1|0,
//                     warls_port=1..65535, wled_sync=1|0; answers like GET, 400 with the problem
//
// Only in the main firmware: main registers the routes.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "px/audio/audio.hpp"
#include "px/webserver/http.hpp"

namespace px::audio {

namespace {

using webserver::detail::form_has;
using webserver::detail::form_value;
using webserver::detail::JsonWriter;
using webserver::detail::read_body;
using webserver::detail::require_login;
using webserver::detail::route;
using webserver::detail::send_error;
using webserver::detail::send_json;

esp_err_t send_state(httpd_req_t *req)
{
    const Status s = status();
    const Settings c = settings();
    char slot[2] = {s.slot, 0};
    static char buffer[1024]; // the server has one task
    JsonWriter json(buffer, sizeof(buffer));
    const Inputs i = inputs();
    json.begin_object()
        .boolean("running", s.running)
        .boolean("microphone", has_microphone())
        .string("source", source_name(s.source))
        .number("packets", s.packets)
        .string("slot", slot)
        .number("blocks", s.blocks)
        .boolean("active", s.audio.active)
        .number("loudness", s.audio.loudness)
        .boolean("beat", s.audio.beat)
        .decimal("loudest_db", static_cast<double>(s.loudest_db))
        .decimal("reference_db", static_cast<double>(s.reference_db))
        .begin_array("bands");
    for (uint8_t band : s.audio.bands) {
        json.number(nullptr, band);
    }
    json.end_array()
        .begin_object("settings")
        .number("gain_db", c.gain_db)
        .boolean("agc", c.agc)
        .number("silence_db", c.silence_db)
        .number("range_db", c.range_db)
        .number("beat_ratio_tenths", c.beat_ratio_tenths)
        .end_object()
        .begin_object("inputs")
        .boolean("warls", i.warls)
        .number("warls_port", i.warls_port)
        .boolean("wled_sync", i.wled_sync)
        .end_object()
        .end_object();
    return send_json(req, json);
}

esp_err_t get_audio(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    return send_state(req);
}

// a whole number from `min` to `max` in the form; false (and 400 sent) when it is there but wrong
bool form_number(httpd_req_t *req, const char *body, const char *key, long min, long max, long &out)
{
    if (!form_has(body, key)) {
        return true;
    }
    char text[16] = {};
    char *end = nullptr;
    if (!form_value(body, key, text, sizeof(text)) || text[0] == 0 || (out = std::strtol(text, &end, 10), *end != 0) ||
        out < min || out > max) {
        char message[64];
        std::snprintf(message, sizeof(message), "%s: a whole number from %ld to %ld", key, min, max);
        send_error(req, "400 Bad Request", message);
        return false;
    }
    return true;
}

// 1 or 0 in the form; false (and 400 sent) when it is there but something else
bool form_switch(httpd_req_t *req, const char *body, const char *key, bool &out)
{
    char text[4] = {};
    if (!form_value(body, key, text, sizeof(text))) {
        return true;
    }
    if (std::strcmp(text, "1") != 0 && std::strcmp(text, "0") != 0) {
        char message[48];
        std::snprintf(message, sizeof(message), "%s: 1 or 0", key);
        send_error(req, "400 Bad Request", message);
        return false;
    }
    out = text[0] == '1';
    return true;
}

esp_err_t post_audio(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    char body[256];
    if (!read_body(req, body, sizeof(body))) {
        return send_error(req, "400 Bad Request", "the request is too large");
    }
    Settings c = settings();
    long gain = c.gain_db, silence = c.silence_db, range = c.range_db, beat = c.beat_ratio_tenths;
    if (!form_number(req, body, "gain_db", kMinGainDb, kMaxGainDb, gain) ||
        !form_number(req, body, "silence_db", kMinSilenceDb, kMaxSilenceDb, silence) ||
        !form_number(req, body, "range_db", kMinRangeDb, kMaxRangeDb, range) ||
        !form_number(req, body, "beat_ratio_tenths", kMinBeatTenths, kMaxBeatTenths, beat)) {
        return ESP_OK;
    }
    bool agc = c.agc;
    Inputs in = inputs();
    long port = in.warls_port;
    if (!form_switch(req, body, "agc", agc) || !form_switch(req, body, "warls", in.warls) ||
        !form_switch(req, body, "wled_sync", in.wled_sync) || !form_number(req, body, "warls_port", 1, 65535, port)) {
        return ESP_OK;
    }
    c.agc = agc;
    c.gain_db = static_cast<int8_t>(gain);
    c.silence_db = static_cast<int8_t>(silence);
    c.range_db = static_cast<uint8_t>(range);
    c.beat_ratio_tenths = static_cast<uint8_t>(beat);
    char error[64] = {};
    if (!set_settings(c, error, sizeof(error))) {
        return send_error(req, "400 Bad Request", error);
    }
    in.warls_port = static_cast<uint16_t>(port);
    const Inputs before = inputs();
    if (before.warls != in.warls || before.warls_port != in.warls_port || before.wled_sync != in.wled_sync) {
        set_inputs(in);
    }
    return send_state(req);
}

} // namespace

void register_routes(void *server)
{
    const httpd_uri_t routes[] = {
        route("/api/audio", HTTP_GET, get_audio),
        route("/api/audio", HTTP_POST, post_audio),
    };
    for (const auto &route : routes) {
        httpd_register_uri_handler(static_cast<httpd_handle_t>(server), &route);
    }
}

} // namespace px::audio
