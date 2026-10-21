#pragma once

// The web server of the device (docs/ui/web/web-app.md, docs/architecture.md section 9.4).
//
// HTTP on port 80, on the Wi-Fi and the hotspot. On the hotspot it is a captive portal: a DNS
// responder answers every name with the hotspot address, the hotspot's DHCP announces the portal
// address (option 114), and a request for any other host is redirected to the portal, which is
// what makes a phone open the setup page by itself.
//
// Static files come from LittleFS (/fs/www, `file.gz` is served gzip encoded). Without them the
// built-in page is served.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "esp_err.h"
#include "px/memory/large.hpp"

namespace px::webserver {

// What a backup import found and did, one line of text each (px/config/backup.hpp).
struct BackupReport {
    // the import is refused when there is one
    memory::Vector<memory::String> problems;
    // unknown sections and fields
    memory::Vector<memory::String> ignored;
    // what the user should know ("network: Office needs its password")
    memory::Vector<memory::String> notes;
    // the sections that were in the file
    memory::Vector<memory::String> sections;
};

// The backup of the settings and the presets in one YAML file, filled in `main` (the recovery
// app has none and passes nullptr; it gets the device back on the network, the import follows in
// the main firmware).
struct BackupAccess {
    // the file with the settings, the presets or both
    memory::String (*export_file)(bool settings, bool presets);
    // checks `text` and, unless `check_only`, imports it when the check found no problem
    BackupReport (*import_file)(std::string_view text, bool check_only);
};

struct Config {
    const char *device_name;
    const BackupAccess *backup = nullptr;
    // registers more routes on the server (an httpd_handle_t), before the files take the rest:
    // the LED API of the main firmware
    void (*add_routes)(void *server) = nullptr;
};

// Starts the server and the DNS responder. The network component has to be started.
esp_err_t start(const Config &config);

// Removes the web UI login (the console, a long press of the button at boot): the web UI is open
// in setup mode until a new login is created.
esp_err_t reset_login();

// The web UI login from a file (the backup file, docs/ui/web/web-app.md section 4): checks `username` and `password` like the setup page, and with `apply` stores them
// in place of the login there is. That ends every session, the browser that sent the file
// included. false with `error` filled when they are not usable or cannot be stored.
bool import_login(const char *username, const char *password, bool apply, char *error, size_t error_size);

// The time zone and the SNTP servers (/api/time, docs/ui/web/web-app.md section 4), for
// Config::add_routes of the main firmware. The server is an httpd_handle_t.
void register_time_routes(void *server);

} // namespace px::webserver
