// The variant of a board (docs/adding-a-board.md "Variants"): one unit's add-ons. A private copy
// of this file in boards/<board>/variants/<name>/ (not in git) changes the fields of the board's
// Info that differ on that unit. tools/build.py <board>.<name> puts the copy on the include path
// instead of this one, which changes nothing. apply() runs at compile time: no cost in the
// firmware.
#pragma once

#include "px/board/board.hpp"

namespace px::board::variant {

constexpr Info apply(Info info)
{
    // info.variant = "kitchen";                   // logged at boot, shown in the System info
    // info.led_gpios = lanes({4, 5, 6, 7});       // the LED data pins of this unit
    // info.power_gpio = 21;                       // relay or MOSFET of the LED supply, -1: none
    // info.power_active_high = false;             // its input low while on
    // info.power_led_gpio = 16;                   // an LED lit with the supply, -1: none
    // info.motion_gpio = 39;                      // a motion sensor, -1: none
    // info.motion_active_high = false;            // its output low while it sees motion
    // info.i2c_sda_gpio = 8;                      // sensor modules
    // info.i2c_scl_gpio = 9;
    // info.light_ldr_gpio = 1;                    // an LDR on an ADC1 pin, -1: none
    // info.mic_sck_gpio = 14;                     // an I2S microphone (INMP441), -1: none
    // info.mic_ws_gpio = 15;
    // info.mic_sd_gpio = 32;
    return info;
}

} // namespace px::board::variant
