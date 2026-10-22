// A no-name ESP32-C3 dev board ("AI-C3" on the back, 2 x 15 pins, 2 USB ports, one RGB LED) for
// testing LED strips: an ESP32-C3-MINI-1 module on a board of its own, so it runs the
// ESP32-C3-DevKitM-1 firmware. In git as an example of a variant.
//
// Read with esptool: ESP32-C3 (QFN32) rev v0.4, 4 MB flash in the chip (XMC, 20/4016), no PSRAM,
// 40 MHz crystal. The board's sdkconfig.defaults fit as they are.
// Not usable: GPIO 18 and 19 (USB), 20 and 21 (UART0, RX/TX), 2, 8 and 9 (strapping; 9 is BOOT).
// The RGB LED sits behind the place "RGB" next to it, most likely on GPIO 8 as on the DevKitM-1
// (not checked), not driven. Photo and pin headers: docs/boards.md.
//
// No sensors, no relay: the 2 LED lanes of the C3's RMT, lane 1 on GPIO 10 (the header with 0 to
// 3 and 10; of those 2 is strapping, 0 and 1 are the 32 kHz crystal pins), lane 2 on GPIO 4 (the
// other header).
// Tested 2026-10-22: strips on both lanes, 2 x 512 LEDs at 60 fps (about 2 x 600 for 50 fps).
#pragma once

#include "px/board/board.hpp"

namespace px::board::variant {

constexpr Info apply(Info info)
{
    info.variant = "ai_c3_test";
    info.led_gpios = lanes({10, 4});
    info.power_gpio = -1;
    info.motion_gpio = -1;
    info.i2c_sda_gpio = -1;
    info.i2c_scl_gpio = -1;
    info.light_ldr_gpio = -1;
    // button 1 stays BOOT (GPIO 9, internal pull-up): held at boot, the hotspot
    return info;
}

} // namespace px::board::variant
