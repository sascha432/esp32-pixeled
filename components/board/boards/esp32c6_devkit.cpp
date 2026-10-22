// Espressif ESP32-C6-DevKitC-1: 8 MB flash (the 4 MB table is used), no PSRAM. Small
// installations: LED lanes out of the RMT, more with PARLIO (docs/leds.md section 2).
// Not usable: GPIO 24 to 30 (flash), 12 and 13 (USB), 16 and 17 (UART0), 4, 5, 8, 9 and 15
// (strapping; 8 drives the board's RGB LED, 9 is the BOOT button).

#include "px/board/board.hpp"

#include <string_view>

#include "px_board_variant.hpp"

// the board tag of every image built for this board (px::board::ImageTag); CMakeLists.txt keeps it
extern "C" constexpr px::board::ImageTag px_image_tag __attribute__((section(".rodata_custom_desc"), used)) = {
    px::board::kImageTagMagic, "ESP32-C6-DevKitC-1"};

namespace px::board {

namespace {

// a unit's add-ons go into a variant (px_board_variant.hpp)
constexpr Info kInfo = variant::apply(Info{
    .name = "ESP32-C6-DevKitC-1",
    .led_gpios = lanes({18, 19, 20, 21}),
    .power_gpio = 22,
    .motion_gpio = 23,
    .i2c_sda_gpio = 6,
    .i2c_scl_gpio = 7,
    .light_ldr_gpio = 1,
    .button_gpios = buttons({9}),
});
// the image tag at the top of this file names the board the same way
static_assert(std::string_view(kInfo.name) == std::string_view(px_image_tag.board));

} // namespace

const Info &info()
{
    return kInfo;
}

} // namespace px::board
