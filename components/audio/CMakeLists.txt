# The audio input: an I2S microphone and its analysis (docs/leds.md section 6.1). analysis.cpp is
# pure C++, built by the host tests as well.
idf_component_register(SRCS "src/analysis.cpp" "src/audio.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES effects
                       PRIV_REQUIRES esp_driver_i2s freertos)
