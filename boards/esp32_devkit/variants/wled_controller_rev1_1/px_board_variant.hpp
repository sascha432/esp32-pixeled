// The WLED controller PCB (https://oshwlab.com/sascha23095123423/wled-controller_copy): an
// ESP32-WROOM-32 with 4 level-shifted LED outputs, a relay for the LED supply, a motion sensor
// input, 4 buttons and a footprint for an INMP441 microphone.
//
// Rev 1.1 of the PCB. Its buttons, push buttons to GND: BTN0 is the hardware reset (EN), not a
// GPIO; BTN1 to BTN3 are buttons 2 to 4 (button 1 stays empty, so they keep the default actions):
//   button 2 = BTN1 = GPIO 35: darker (no internal pull-up: the PCB's)
//   button 3 = BTN2 = GPIO 34: brighter (no internal pull-up: the PCB's)
//   button 4 = BTN3 = GPIO 19: on/off; held while the device starts (with BTN0, the reset): the
//              hotspot
// The GPIOs are those of rev 1.3 (its schematic: BTN0 GPIO 13, a 10k pull-up and 100 nF on each);
// check them on rev 1.1 with the console's `input`. A rotary encoder is not fitted: its pins stay
// -1 until one is wired (free: 17, 18, 23).
//
// From the netlist (PCB export 2026-10-21): GPIO 4 is IR_RX (the IR receiver footprint). Not
// connected: 2, 5, 12, 17, 18, 23 (2, 5 and 12 are strapping pins). Only lanes 1 to 4 go through
// the level shifter (U6).
#pragma once

#include "px/board/board.hpp"

namespace px::board::variant {

constexpr Info apply(Info info)
{
    info.variant = "wled_controller_rev1_1";
    // the 8-lane timing test (2026-10-21, docs/leds.md section 2.2) added 17, 18, 23 and 5
    info.led_gpios = lanes({27, 26, 25, 33});
    info.power_gpio = 21;
    // the supply is on while GPIO 21 is high (tested 2026-10-19 with the strip; the old notes said
    // inverted). Rev 1.1 has no LED on a GPIO: its indicator LED hangs on the MOSFET.
    info.power_active_high = true;
    info.power_led_gpio = -1;
    info.motion_gpio = 22;
    // 21 and 22 are the relay and the motion sensor: no I2C bus, no LDR
    info.i2c_sda_gpio = -1;
    info.i2c_scl_gpio = -1;
    info.light_ldr_gpio = -1;
    info.button_gpios = buttons({-1, 35, 34, 19});
    info.hotspot_button = 3;
    // the INMP441, L/R to GND (mono)
    info.mic_sck_gpio = 14;
    info.mic_ws_gpio = 15;
    info.mic_sd_gpio = 32;
    return info;
}

} // namespace px::board::variant
