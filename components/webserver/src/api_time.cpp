// The clock in the web API (docs/ui/web/web-app.md section 4), login required:
//
//   GET  /api/time        {"synced", "now", "local", "zone", "last_sync", "servers", "defaults"}:
//                         now and last_sync in Unix time (0: never), local as "2026-10-21 14:03:07"
//   POST /api/time        any of zone (an IANA name, empty: UTC), server1, server2 (empty: none,
//                         both empty: the defaults); answers like GET, 400 with the problem
//   GET  /api/time/zones  the IANA names the firmware knows, sorted: ["Africa/Abidjan", ...]
//
// Only in the main firmware: main registers the routes (register_time_routes); the recovery app
// keeps no time.

#include <cstdio>
#include <cstring>
#include <ctime>

#include "px/platform/time_sync.hpp"
#include "px/platform/timezone.hpp"
#include "px/webserver/webserver.hpp"

#include "http_util.hpp"

namespace px::webserver {

namespace {

using detail::form_has;
using detail::JsonWriter;
using detail::form_value;
using detail::read_body;
using detail::require_login;
using detail::route;
using detail::send_error;
using detail::send_json;

esp_err_t send_settings(httpd_req_t *req)
{
    const std::time_t now = std::time(nullptr);
    const bool synced = platform::time_synced();
    char local[24] = {};
    if (synced) {
        std::tm tm{};
        localtime_r(&now, &tm);
        std::strftime(local, sizeof(local), "%Y-%m-%d %H:%M:%S", &tm);
    }
    char zone[platform::kMaxZoneLength] = {};
    platform::time_zone(zone, sizeof(zone));
    const platform::TimeServers servers = platform::time_servers();

    char buffer[512];
    JsonWriter json(buffer, sizeof(buffer));
    json.begin_object()
        .boolean("synced", synced)
        .number("now", synced ? now : 0)
        .string("local", local)
        .string("zone", zone)
        .number("last_sync", platform::last_time_sync())
        .begin_array("servers");
    for (const char *name : servers.names) {
        json.string(nullptr, name);
    }
    json.end_array().begin_array("defaults");
    for (const char *name : platform::kDefaultTimeServers) {
        json.string(nullptr, name);
    }
    json.end_array().end_object();
    return send_json(req, json);
}

esp_err_t get_time(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    return send_settings(req);
}

esp_err_t post_time(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    char body[320];
    if (!read_body(req, body, sizeof(body))) {
        return send_error(req, "400 Bad Request", "the request is too large");
    }
    // both checked before either changes
    char zone[platform::kMaxZoneLength] = {};
    const bool has_zone = form_has(body, "zone");
    if (has_zone && (!form_value(body, "zone", zone, sizeof(zone)) ||
                     (zone[0] != 0 && platform::time_zone_rule(zone) == nullptr))) {
        return send_error(req, "400 Bad Request", "zone: not a time zone this firmware knows");
    }
    platform::TimeServers servers = platform::time_servers();
    bool has_servers = false;
    const char *keys[platform::kTimeServers] = {"server1", "server2"};
    for (size_t i = 0; i < platform::kTimeServers; ++i) {
        if (!form_has(body, keys[i])) {
            continue;
        }
        has_servers = true;
        if (!form_value(body, keys[i], servers.names[i], sizeof(servers.names[i]))) {
            char message[48];
            std::snprintf(message, sizeof(message), "%s: at most %u characters", keys[i],
                          static_cast<unsigned>(platform::kTimeServerSize - 1));
            return send_error(req, "400 Bad Request", message);
        }
    }
    char error[96] = {};
    if (has_servers && !platform::check_time_servers(servers, error, sizeof(error))) {
        return send_error(req, "400 Bad Request", error);
    }
    if (has_zone) {
        platform::set_time_zone(zone);
    }
    if (has_servers && !platform::set_time_servers(servers, error, sizeof(error))) {
        return send_error(req, "500 Internal Server Error", error);
    }
    return send_settings(req);
}

// about 13 KB: sent in pieces (the names need no escaping)
esp_err_t get_zones(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char chunk[1024];
    size_t length = 0;
    chunk[length++] = '[';
    const size_t count = platform::time_zone_count();
    for (size_t i = 0; i < count; ++i) {
        const std::string_view name = platform::time_zone_name(i);
        if (length + name.size() + 4 > sizeof(chunk)) {
            if (httpd_resp_send_chunk(req, chunk, static_cast<ssize_t>(length)) != ESP_OK) {
                return ESP_FAIL;
            }
            length = 0;
        }
        if (i > 0) {
            chunk[length++] = ',';
        }
        chunk[length++] = '"';
        std::memcpy(chunk + length, name.data(), name.size());
        length += name.size();
        chunk[length++] = '"';
    }
    chunk[length++] = ']';
    if (httpd_resp_send_chunk(req, chunk, static_cast<ssize_t>(length)) != ESP_OK) {
        return ESP_FAIL;
    }
    return httpd_resp_send_chunk(req, nullptr, 0);
}

} // namespace

void register_time_routes(void *server)
{
    const httpd_uri_t routes[] = {
        route("/api/time", HTTP_GET, get_time),
        route("/api/time", HTTP_POST, post_time),
        route("/api/time/zones", HTTP_GET, get_zones),
    };
    for (const auto &route : routes) {
        httpd_register_uri_handler(static_cast<httpd_handle_t>(server), &route);
    }
}

} // namespace px::webserver
