#pragma once

namespace px::platform {

// esp_restart() with a deadline: the RTC watchdog resets the chip kRestartDeadlineMs from now,
// should a shutdown handler (pending settings, Wi-Fi, the LED supply) or esp_restart itself wait
// for good (seen once in hassctrl with 10 KB RAM free: Wi-Fi stopped, the board stayed up). The
// next start then reports a watchdog reset instead of a software reset. Every restart of the
// firmware goes through here.
constexpr unsigned kRestartDeadlineMs = 5000;

[[noreturn]] void restart();

} // namespace px::platform
