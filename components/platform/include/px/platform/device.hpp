#pragma once

namespace px::platform {

// "pixeled-" and the last two bytes of the Wi-Fi MAC address in hex, for example
// "pixeled-1a2b": the hotspot name and the host name of the device.
const char *device_name();

} // namespace px::platform
