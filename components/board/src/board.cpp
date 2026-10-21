// The bring-up every board shares (px/board/board.hpp); the board files in boards/ only hold
// their constants.

#include "px/board/board.hpp"

#include "esp_log.h"

namespace px::board {

namespace {

constexpr const char *kTag = "board";

void init_i2c(const Info &board, Handles &out)
{
    if (board.i2c_sda_gpio < 0 || board.i2c_scl_gpio < 0) {
        return;
    }
    i2c_master_bus_config_t bus{};
    bus.i2c_port = I2C_NUM_0;
    bus.sda_io_num = static_cast<gpio_num_t>(board.i2c_sda_gpio);
    bus.scl_io_num = static_cast<gpio_num_t>(board.i2c_scl_gpio);
    bus.clk_source = I2C_CLK_SRC_DEFAULT;
    bus.glitch_ignore_cnt = 7;
    // modules on a cable usually bring their own pullups; the internal ones help without
    bus.flags.enable_internal_pullup = true;
    if (const esp_err_t err = i2c_new_master_bus(&bus, &out.i2c); err != ESP_OK) {
        ESP_LOGW(kTag, "I2C bus on SDA %d, SCL %d did not start: %s", board.i2c_sda_gpio, board.i2c_scl_gpio,
                 esp_err_to_name(err));
        out.i2c = nullptr;
    }
}

} // namespace

esp_err_t init(Handles &out)
{
    out = {};
    init_i2c(info(), out);
    return ESP_OK;
}

} // namespace px::board
