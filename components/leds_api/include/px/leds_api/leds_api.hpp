#pragma once

// The LEDs in the web API (docs/ui/web/web-app.md section 4): the routes of /api/leds, registered
// by main through webserver::Config::add_routes, so the web server does not depend on the LEDs
// (the recovery app has none).

namespace px::leds_api {

// `server` is the httpd_handle_t the web server hands to add_routes
void register_routes(void *server);

} // namespace px::leds_api
