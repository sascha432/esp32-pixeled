#pragma once

// The web server's own parts of the API handlers; the helpers every handler uses are in
// px/webserver/http.hpp.

#include <cstddef>

#include "esp_http_server.h"
#include "px/formats/json_writer.hpp"
#include "px/webserver/http.hpp"

#include "auth.hpp"

namespace px::webserver {
struct BackupAccess;
}

namespace px::webserver::detail {

using formats::JsonWriter;

constexpr const char *kSessionCookie = "px_session";

// the token of the session cookie, false without one
bool session_token(httpd_req_t *req, char (&token)[auth::kTokenSize]);

// ------------------------------------------------------------------------- sessions (idle.cpp)

// The server's open_fn and close_fn: the time of every session's last byte, sent or received; a
// session silent for 30 s is closed (start_idle_close), the WebSockets and a long response on a
// task of its own (px/webserver/async.hpp) excepted. Every byte received is activity for the
// Wi-Fi's modem sleep.
esp_err_t open_session(httpd_handle_t server, int fd);
void close_session(httpd_handle_t server, int fd);
void start_idle_close(httpd_handle_t server);

// ------------------------------------------------------------------- live socket (live.cpp)

// GET /api/live, the WebSocket of the web app (add_live_topic in px/webserver/http.hpp). Topics
// may be added before or after it.
void register_live_routes(httpd_handle_t server);
// the session closed: its subscription goes (close_session)
void live_forget(int fd);
// closes the live sockets of the session with this token (a logout)
void live_close_token(const char *token);

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
