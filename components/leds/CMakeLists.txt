# The LED engine: the render task (docs/leds.md section 3). frame.cpp is pure C++, built by the
# host tests as well.
idf_component_register(SRCS "src/frame.cpp" "src/leds.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES effects layout output
                       PRIV_REQUIRES esp_timer freertos memory platform)
