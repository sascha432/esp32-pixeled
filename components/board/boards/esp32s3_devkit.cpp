// Espressif ESP32-S3-DevKitC-1 N16R8: 16 MB flash, 8 MB octal PSRAM. The main target: 8 LED lanes
// out of the LCD peripheral, 16 with a variant (docs/leds.md section 2).
// Not usable: GPIO 26 to 32 (flash), 33 to 37 (octal PSRAM), 19 and 20 (USB), 43 and 44 (UART0),
// 0, 3, 45 and 46 (strapping; 0 is the BOOT button).

#include "px/board/board.hpp"

#include <string_view>

#include "px_board_variant.hpp"

// the board tag of every image built for this board (px::board::ImageTag); CMakeLists.txt keeps it
extern "C" constexpr px::board::ImageTag px_image_tag __attribute__((section(".rodata_custom_desc"), used)) = {
    px::board::kImageTagMagic, "ESP32-S3-DevKitC-1"};

namespace px::board {

namespace {

// a unit's add-ons go into a variant (px_board_variant.hpp)
constexpr Info kInfo = variant::apply(Info{
    .name = "ESP32-S3-DevKitC-1",
    .led_gpios = lanes({4, 5, 6, 7, 15, 16, 17, 18}),
    .power_gpio = 21,
    .motion_gpio = 39,
    .i2c_sda_gpio = 8,
    .i2c_scl_gpio = 9,
    .light_ldr_gpio = 1,
    .button_gpio = 0,
});
// the image tag at the top of this file names the board the same way
static_assert(std::string_view(kInfo.name) == std::string_view(px_image_tag.board));

} // namespace

const Info &info()
{
    return kInfo;
}

} // namespace px::board
