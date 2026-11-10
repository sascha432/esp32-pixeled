// ESP32-C6 SuperMini (purple, a small no-name board): an ESP32-C6FH4 chip, one USB-C port (the C6's
// own USB on GPIO 12 and 13, no USB-UART chip), BOOT and RST buttons, a WS2812 on GPIO 8 and an
// LED on GPIO 15 (not checked). In git as an example of a variant for a board with only the chip's
// own USB: its sdkconfig.defaults moves the console there. Photo, pins: docs/boards.md.
//
// Read with esptool 2026-11-09: ESP32-C6FH4 (QFN32) rev v0.2, 4 MB flash in the chip, no PSRAM,
// 40 MHz crystal, so it runs esp32c6_devkit (the 4 MB table). Came with an Arduino sketch.
//
// Lane 1 is the board's WS2812 on GPIO 8, for a first test without a strip (a strapping pin: the
// RMT lets go of it at a reset; the WS2812 keeps its last color while it has power). The other
// pins are still those of the ESP32-C6-DevKitC-1 (lanes 2 to 4 on 19 to 21, supply 22, motion 23,
// I2C 6 and 7, LDR 1, button 9 = BOOT); which of them reach the board's edge is to be checked.
// The C6 sends 2 lanes (RMT) until PARLIO is built.
// Tested 2026-11-09: recovery, main and web UI, the console on the USB port, the WS2812 as lane 1.
#pragma once

#include "px/board/board.hpp"

namespace px::board::variant {

constexpr Info apply(Info info)
{
    info.variant = "supermini";
    info.led_gpios = lanes({8, 19, 20, 21});
    return info;
}

} // namespace px::board::variant
