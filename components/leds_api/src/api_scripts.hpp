#pragma once

// The routes of the scripts (api_scripts.cpp, CONFIG_PX_SCRIPTS), registered with the LED routes.

#include <cstddef>

#include "px/webserver/http.hpp"

namespace px::leds_api {

void register_script_routes(void *server);

// The params of the script in `slot` into the open array (for the one shown, not for every script
// of a list): the label, the slider and its value; `full` with the name and the range too. From
// its loaded program while it runs (RAM), else from its file.
void write_script_params(webserver::detail::JsonWriter &json, size_t slot, bool full);

} // namespace px::leds_api
