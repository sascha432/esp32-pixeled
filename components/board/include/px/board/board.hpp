#pragma once

// Board support (docs/architecture.md section 8): the constants of the board the firmware is
// built for and the bring-up of its shared buses. The pins are the defaults of a new device; the
// LED and hardware settings start from them and can change them at runtime
// (docs/architecture.md section 8.1).

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>

#include "driver/i2c_master.h"
#include "esp_err.h"

namespace px::board {

// LED outputs (lanes) the firmware handles at most: the 16 data lines of the ESP32-S3's LCD
// peripheral (docs/leds.md section 2)
constexpr size_t kMaxLanes = 16;

using LanePins = std::array<int8_t, kMaxLanes>;

// `pins` in lane order, -1 for the lanes after them
constexpr LanePins lanes(std::initializer_list<int8_t> pins)
{
    LanePins out{};
    out.fill(-1);
    size_t i = 0;
    for (const int8_t pin : pins) {
        if (i < out.size()) {
            out[i++] = pin;
        }
    }
    return out;
}

// Push buttons the firmware handles (px::input): the board's own and up to three more
constexpr size_t kMaxButtons = 4;

using ButtonPins = std::array<int8_t, kMaxButtons>;

// `pins` in button order, -1 for the buttons after them
constexpr ButtonPins buttons(std::initializer_list<int8_t> pins)
{
    ButtonPins out{};
    out.fill(-1);
    size_t i = 0;
    for (const int8_t pin : pins) {
        if (i < out.size()) {
            out[i++] = pin;
        }
    }
    return out;
}

// The constants of a board type. A variant of the board, one unit's add-ons (a relay, a motion
// sensor, other LED pins), is a private copy of px_board_variant.hpp in
// boards/<board>/variants/<name>/ whose apply() changes the fields below (docs/adding-a-board.md
// "Variants"). -1 is a pin that is not there.
struct Info {
    const char *name;
    // the variant this firmware was built for (px::board::variant::apply), nullptr: the board as
    // it is in the repository
    const char *variant = nullptr;
    // the data pins of the LED lanes of a new device, lane 1 first
    LanePins led_gpios = lanes({});
    // The relay or MOSFET of the LED power supply (px::relay): on before the first frame, off
    // after the LEDs faded out, off while the board starts.
    int8_t power_gpio = -1;
    bool power_active_high = true;
    // An LED that shows the supply is on (high = lit), switched with it, -1: none
    int8_t power_led_gpio = -1;
    // A motion sensor (PIR or radar), its output level while it sees motion (px::motion)
    int8_t motion_gpio = -1;
    bool motion_active_high = true;
    // The I2C bus for sensor modules (BH1750, temperature), -1: none
    int8_t i2c_sda_gpio = -1;
    int8_t i2c_scl_gpio = -1;
    // what the BH1750 measures into the light of the room (behind a diffuser: above 1)
    float light_sensor_scale = 1.0f;
    // An LDR on this ADC1 GPIO (LDR to 3V3, 10 kOhm to GND) when no BH1750 answers, -1: none
    int8_t light_ldr_gpio = -1;
    // Push buttons to GND, low while pressed (px::input): button 1 is the board's own (BOOT on the
    // dev kits). The internal pull-up is used where the pin has one (not GPIO 34 to 39 of the
    // ESP32: those need one on the board). -1: none (a button that is not there keeps its number,
    // so the others keep theirs and their actions)
    ButtonPins button_gpios = buttons({});
    // the button (0: button 1) that, held while the device starts, opens the hotspot
    uint8_t hotspot_button = 0;
    // A rotary encoder (px::input): its A and B pins (to GND at the detents, pull-ups) and its push
    // switch (low while pressed), -1: none
    int8_t encoder_a_gpio = -1;
    int8_t encoder_b_gpio = -1;
    int8_t encoder_button_gpio = -1;
    // An I2S microphone (INMP441 or alike: L/R to GND), its clock, word select and data pins,
    // -1: none (px::audio, docs/leds.md section 6.1)
    int8_t mic_sck_gpio = -1;
    int8_t mic_ws_gpio = -1;
    int8_t mic_sd_gpio = -1;
};

// The board an image was built for, at a fixed place in every firmware image: ESP-IDF's custom
// app description, right after esp_app_desc_t (section .rodata_custom_desc). An update is checked
// against the running board with its first bytes, before anything is written (px::ota).
struct ImageTag {
    uint32_t magic;
    char board[32];
};
constexpr uint32_t kImageTagMagic = 0x54425850; // "PXBT" in the image

struct Handles {
    // the board's I2C bus for sensor modules, nullptr without one
    i2c_master_bus_handle_t i2c;
};

const Info &info();

// Brings up the board's shared buses. Call once. A bus that does not start is logged and left
// out (its handle stays nullptr).
esp_err_t init(Handles &out);

} // namespace px::board
