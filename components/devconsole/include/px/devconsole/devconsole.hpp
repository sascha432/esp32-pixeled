#pragma once

// A command line on the serial port for development (CONFIG_PX_DEV_CONSOLE). Type `help` for the
// commands.
//
// The console is created early, in a task of its own that does not hold up app_main (the terminal
// probe and the drain of the boot log take seconds), while the internal RAM is in one piece. The
// stored log level is applied then as well. Commands are taken once ready() says the components
// they use run.

#include "esp_err.h"

namespace px::devconsole {

// Call right after the storage is initialized; returns at once.
esp_err_t start();

// The network and the other components run: the console takes commands from now on.
void ready();

} // namespace px::devconsole
