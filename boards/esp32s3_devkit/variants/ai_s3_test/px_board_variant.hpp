// A no-name ESP32-S3 dev board ("AI S3" on the back, 44 pins, 2 USB ports, one RGB LED) for
// testing LED strips: a clone of the ESP32-S3-DevKitC-1 N16R8. In git as an example of a variant.
//
// Read with esptool: ESP32-S3 (QFN56) rev v0.1, 16 MB quad flash (GigaDevice c8/4018, 3.3 V),
// 8 MB embedded octal PSRAM (S3R8), 40 MHz crystal. The board's sdkconfig.defaults fit as they are.
// Not usable: GPIO 26 to 32 (flash), 33 to 37 (octal PSRAM), 19 and 20 (USB), 43 and 44 (UART0),
// 0, 3, 45 and 46 (strapping). The RGB LED is a WS2812 on GPIO 48 (connected through a 0 Ohm
// resistor at "RGB"), not driven. Photo and pin headers: docs/boards.md.
//
// No sensors, no relay: only the 8 LED lanes, on the pins that sit next to each other on the left
// header (after 3V3, 3V3, RST): 4, 5, 6, 7, 15, 16, 17, 18.
// Lane 1 (GPIO 4) tested with a strip. The pin "5Vin" carries no USB 5 V while the jumper
// "IN-OUT" is open (as delivered): feed the strip from its own supply.
#pragma once

#include "px/board/board.hpp"

namespace px::board::variant {

constexpr Info apply(Info info)
{
    info.variant = "ai_s3_test";
    info.led_gpios = lanes({4, 5, 6, 7, 15, 16, 17, 18});
    info.power_gpio = -1;
    info.motion_gpio = -1;
    info.i2c_sda_gpio = -1;
    info.i2c_scl_gpio = -1;
    info.light_ldr_gpio = -1;
    // button 1 stays BOOT (GPIO 0, internal pull-up): held at boot, the hotspot
    return info;
}

} // namespace px::board::variant
