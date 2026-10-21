#pragma once

// The helpers of the API handlers, for route modules outside the web server (registered through
// webserver::Config::add_routes, docs/architecture.md section 9.4). Request bodies are HTML form
// encoded (application/x-www-form-urlencoded), answers are JSON. Every handler starts with
// `if (!require_login(req)) return ESP_OK;`.

#include <cstddef>

#include "esp_http_server.h"
#include "px/formats/json_writer.hpp"

namespace px::webserver::detail {

using formats::JsonWriter;

// Reads the whole body into `out` (terminated). False when it is larger than the buffer.
bool read_body(httpd_req_t *req, char *out, size_t size);

// The whole request body in a buffer of the large heap (memory::release_large), terminated; null
// when it is empty, larger than `limit` or the client stalls (the server has one task, one
// receive timeout ends the request).
char *read_large_body(httpd_req_t *req, size_t limit, size_t &length);

// the value of `key` in the query string, false when it is not there
bool query_value(httpd_req_t *req, const char *key, char *out, size_t size);

// The decoded value of `key` in a form encoded body; false when the key is missing or the value
// does not fit.
bool form_value(const char *body, const char *key, char *out, size_t size);

// true when the form has `key`, also when its value would not fit (form_value fails then): a value
// that is too long is not a missing one
bool form_has(const char *body, const char *key);

esp_err_t send_json(httpd_req_t *req, const JsonWriter &json);
// {"error": message} with an HTTP status such as "401 Unauthorized"
esp_err_t send_error(httpd_req_t *req, const char *status, const char *message);

// A route for httpd_register_uri_handler. The struct has more fields with WebSocket support
// (CONFIG_HTTPD_WS_SUPPORT, the main firmware): all zero here.
inline httpd_uri_t route(const char *uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *),
                         void *user_ctx = nullptr)
{
    httpd_uri_t out{};
    out.uri = uri;
    out.method = method;
    out.handler = handler;
    out.user_ctx = user_ctx;
    return out;
}

// Setup mode (no login yet) or a valid session cookie. Sends 401 and returns false otherwise.
bool require_login(httpd_req_t *req);
// The same without an answer: for a WebSocket, whose handshake is answered before the handler runs.
bool logged_in(httpd_req_t *req);

} // namespace px::webserver::detail
