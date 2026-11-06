// The backup of the settings and the presets in one YAML file (docs/TODO.md, "Settings export
// and import"), login required:
//
//   GET  /api/backup.yaml[?parts=settings|presets]
//                                       the backup file, both parts without `parts`; never a
//                                       password or a token
//   POST /api/backup/import[?check=1]   body: the backup file (a password or a token added by
//                                       hand is used); check=1 only checks. Answer:
//                                       {"checked", "imported", "sections": [...], "problems": [...],
//                                       "ignored": [...], "notes": [...]}, 400 when a problem refused it
//
// Only in the main firmware: the recovery app has no backup and does not register these.

#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "px/memory/large.hpp"
#include "px/webserver/webserver.hpp"

#include "http_util.hpp"

namespace px::webserver::detail {

namespace {

const BackupAccess *g_backup = nullptr;
const char *g_device_name = "";

// settings, presets and playlists: a few dozen KB
constexpr size_t kMaxBackup = 256 * 1024;

esp_err_t get_backup(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    char parts[16] = {};
    const bool chosen = query_value(req, "parts", parts, sizeof(parts));
    const bool settings = !chosen || std::strcmp(parts, "settings") == 0;
    const bool presets = !chosen || std::strcmp(parts, "presets") == 0;
    if (!settings && !presets) {
        return send_error(req, "400 Bad Request", "parts: settings or presets");
    }
    char disposition[96];
    std::snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s-%s.yaml\"", g_device_name,
                  settings && presets ? "backup" : settings ? "settings" : "presets");
    httpd_resp_set_type(req, "application/yaml; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);
    // in chunks as it is written; a client that went away gets no more
    struct Out {
        httpd_req_t *req;
        esp_err_t err;
    } out{req, ESP_OK};
    g_backup->export_file(
        settings, presets,
        [](std::string_view piece, void *context) {
            auto &o = *static_cast<Out *>(context);
            if (o.err == ESP_OK) {
                o.err = httpd_resp_send_chunk(o.req, piece.data(), static_cast<ssize_t>(piece.size()));
            }
        },
        &out);
    return out.err == ESP_OK ? httpd_resp_send_chunk(req, nullptr, 0) : out.err;
}

void lines(JsonWriter &json, const char *key, const memory::Vector<memory::String> &values)
{
    json.begin_array(key);
    for (const memory::String &value : values) {
        json.string(nullptr, value.c_str(), value.size());
    }
    json.end_array();
}

esp_err_t post_import(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    char check[8] = {};
    const bool check_only = query_value(req, "check", check, sizeof(check)) && std::strcmp(check, "1") == 0;
    size_t length = 0;
    const auto read = read_large_body(req, kMaxBackup, length);
    if (!read) {
        return read.error() == ESP_ERR_NO_MEM
                   ? send_error(req, "503 Service Unavailable", "no memory for the file")
                   : send_error(req, "400 Bad Request", "the request is empty or larger than 256 KB");
    }
    char *body = *read;
    // The file is read into a tree of strings, which cannot fail softly (3.1 times its size at the
    // most, measured with a 32-bit build for a backup of 19 KB with 12 scripts and one of 4.6 KB),
    // and a script in it is compiled (23 KB): a file the free memory cannot hold that way is refused
    // rather than stop the device (an ESP32 without PSRAM, a backup with many scripts)
    const size_t need = length * 7 / 2 + 28 * 1024;
    const size_t free_bytes = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    if (free_bytes < need) {
        memory::release_large(body);
        char message[112];
        std::snprintf(message, sizeof(message), "not enough memory to read a backup of %u KB: about %u KB needed, %u KB free",
                      static_cast<unsigned>(length / 1024 + 1), static_cast<unsigned>(need / 1024),
                      static_cast<unsigned>(free_bytes / 1024));
        return send_error(req, "503 Service Unavailable", message);
    }
    const BackupReport report = g_backup->import_file(std::string_view(body, length), check_only);
    memory::release_large(body);

    const bool refused = !report.problems.empty();
    const auto write = [&](JsonWriter &json) {
        json.begin_object().boolean("checked", check_only).boolean("imported", !check_only && !refused);
        lines(json, "sections", report.sections);
        lines(json, "problems", report.problems);
        lines(json, "ignored", report.ignored);
        lines(json, "notes", report.notes);
        json.end_object();
    };
    JsonWriter measure(nullptr, 0);
    write(measure);
    auto *buffer = static_cast<char *>(memory::allocate_large(measure.needed() + 1));
    if (buffer == nullptr) {
        // done (or refused) already: only the report is missing
        return send_error(req, "503 Service Unavailable",
                          refused      ? "not imported, the file has problems; no memory to list them"
                          : check_only ? "the file is fine; no memory for the report"
                                       : "imported; no memory for the report");
    }
    JsonWriter json(buffer, measure.needed() + 1);
    write(json);
    if (refused) {
        httpd_resp_set_status(req, "400 Bad Request");
    }
    const esp_err_t err = send_json(req, json);
    memory::release_large(buffer);
    return err;
}

} // namespace

void register_backup_routes(httpd_handle_t server, const BackupAccess *backup, const char *device_name)
{
    if (backup == nullptr) {
        return;
    }
    g_backup = backup;
    g_device_name = device_name;
    const httpd_uri_t routes[] = {
        route("/api/backup.yaml", HTTP_GET, get_backup),
        route("/api/backup/import", HTTP_POST, post_import),
    };
    for (const auto &route : routes) {
        httpd_register_uri_handler(server, &route);
    }
}

} // namespace px::webserver::detail
