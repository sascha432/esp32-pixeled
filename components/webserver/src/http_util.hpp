#pragma once

// The web server's own parts of the API handlers; the helpers every handler uses are in
// px/webserver/http.hpp.

#include <cstddef>

#include "esp_http_server.h"
#include "px/formats/json_writer.hpp"
#include "px/webserver/http.hpp"

namespace px::webserver {
struct BackupAccess;
}

namespace px::webserver::detail {

using formats::JsonWriter;

constexpr const char *kSessionCookie = "px_session";

// a session cookie for the user; `remember` makes it last 90 days
bool set_session_cookie(httpd_req_t *req, const char *username, bool remember);
void clear_session_cookie(httpd_req_t *req);

void register_session_routes(httpd_handle_t server);
void register_wifi_routes(httpd_handle_t server);
void register_update_routes(httpd_handle_t server);
void register_file_routes(httpd_handle_t server);
void register_log_routes(httpd_handle_t server);
// nothing without a backup (the recovery app)
void register_backup_routes(httpd_handle_t server, const BackupAccess *backup, const char *device_name);

} // namespace px::webserver::detail
