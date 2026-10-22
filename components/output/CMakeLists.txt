# The LED outputs (docs/leds.md section 2): the RMT backend, and the parallel backends of the ESP32
# (I2S) and the ESP32-S3 (LCD_CAM).
idf_component_register(SRCS "src/output.cpp" "src/rmt_output.cpp" "src/i2s_output.cpp" "src/lcd_output.cpp"
                            "src/parallel_none.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_gpio esp_driver_rmt esp_driver_dma esp_hal_i2s esp_hal_lcd esp_hal_dma
                                     esp_hw_support esp_rom heap)
