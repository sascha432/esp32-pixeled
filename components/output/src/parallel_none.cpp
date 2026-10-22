// The parallel backend of px::output on the chips without one (C3, C6 until PARLIO is built):
// px::output sends with RMT instead.

#include "backends.hpp"

#include "sdkconfig.h"

#if !CONFIG_IDF_TARGET_ESP32 && !CONFIG_IDF_TARGET_ESP32S3

namespace px::output::detail::parallel {

bool supported()
{
    return false;
}

esp_err_t start(const Lane *, size_t)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void stop()
{
}

esp_err_t show(const uint8_t *, uint32_t)
{
    return ESP_ERR_NOT_SUPPORTED;
}

uint32_t encoder_max_us()
{
    return 0;
}

std::expected<uint32_t, esp_err_t> encoder_test()
{
    return std::unexpected(ESP_ERR_NOT_SUPPORTED);
}

} // namespace px::output::detail::parallel

#endif
