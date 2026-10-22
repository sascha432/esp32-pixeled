// Espressif ESP32-DevKitC with an ESP32-WROOM-32: 4 MB flash, no PSRAM. 8 LED lanes out of the
// I2S peripheral in LCD mode (docs/leds.md section 2).
// Not usable: GPIO 6 to 11 (flash), 1 and 3 (UART0), 0, 2, 5, 12 and 15 (strapping; 0 is the BOOT
// button); 34 to 39 are inputs only.

#include "px/board/board.hpp"

#include <string_view>

#include "px_board_variant.hpp"

// the board tag of every image built for this board (px::board::ImageTag); CMakeLists.txt keeps it
extern "C" constexpr px::board::ImageTag px_image_tag __attribute__((section(".rodata_custom_desc"), used)) = {
    px::board::kImageTagMagic, "ESP32-DevKitC"};

namespace px::board {

namespace {

// a unit's add-ons go into a variant (px_board_variant.hpp)
constexpr Info kInfo = variant::apply(Info{
    .name = "ESP32-DevKitC",
    .led_gpios = lanes({13, 14, 16, 17, 18, 19, 23, 25}),
    .power_gpio = 26,
    .motion_gpio = 34,
    .i2c_sda_gpio = 21,
    .i2c_scl_gpio = 22,
    .light_ldr_gpio = 36,
    .button_gpios = buttons({0}),
});
// the image tag at the top of this file names the board the same way
static_assert(std::string_view(kInfo.name) == std::string_view(px_image_tag.board));

} // namespace

const Info &info()
{
    return kInfo;
}

} // namespace px::board
