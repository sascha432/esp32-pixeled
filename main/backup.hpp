#pragma once

// The backup file of the main firmware (px/config/backup.hpp): the sections network, leds and
// webui (import only), for the web server. Wiring only, like the rest of main.

#include "px/webserver/webserver.hpp"

const px::webserver::BackupAccess &backup_access();
