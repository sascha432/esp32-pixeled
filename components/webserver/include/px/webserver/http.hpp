#pragma once

// The helpers of the API handlers, for route modules outside the web server (registered through
// webserver::Config::add_routes, docs/architecture.md section 9.4). Request bodies are HTML form
// encoded (application/x-www-form-urlencoded), answers are JSON. Every handler starts with
// `if (!require_login(req)) return ESP_OK;`.

#include <cstddef>

#include "esp_http_server.h"
#include "px/formats/json_writer.hpp"
#include "px/memory/large.hpp"

namespace px::webserver::detail {

using formats::JsonWriter;

// Reads the whole body into `out` (terminated). False when it is larger than the buffer.
bool read_body(httpd_req_t *req, char *out, size_t size);

// The whole request body in a buffer of the large heap (memory::release_large), terminated; null
// when it is empty, larger than `limit` or the client stalls (one receive timeout, 5 s: a form
// arrives at once).
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

// The answer of a GET that the live socket pushes as well (live.cpp, docs/ui/web/web-app.md
// section 4.1): writes the whole state, called once to measure and once to write (twice more if
// the state grew in between). It runs on the server's task, also without a request (the live
// socket's tick), so it reads only copies of other tasks' state.
using StateWriter = void (*)(JsonWriter &json);
// the state's JSON in `buffer` (the large heap), its length; 0 when it kept growing
size_t build_state(StateWriter write, memory::Vector<char> &buffer);
// GET answered with the state (no-store), 500 when it could not be built
esp_err_t send_state(httpd_req_t *req, StateWriter write);
// The live socket follows the GET `path` (at most 31 characters): every `period_ms` its state is
// built and sent to the tabs that follow it when it changed. Called by the route files with their
// routes; nothing in the recovery app (no WebSocket support).
void add_live_topic(const char *path, uint32_t period_ms, StateWriter write);

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
