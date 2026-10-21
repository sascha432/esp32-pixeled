#pragma once

namespace px::webserver::detail {

// The page served at / while no web UI is installed on LittleFS: device name and live status.
// Becomes the recovery page (login, update, export) in a later step (docs/ui/web/web-recovery.md).
extern const char kBuiltinPage[];

} // namespace px::webserver::detail
