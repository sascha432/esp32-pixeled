# The LED outputs (docs/leds.md section 2): the RMT backend, and the I2S parallel backend of the
# ESP32.
idf_component_register(SRCS "src/output.cpp" "src/rmt_output.cpp" "src/i2s_output.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_gpio esp_driver_rmt esp_hal_i2s esp_hw_support esp_rom heap)
