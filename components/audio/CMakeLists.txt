# The audio input: an I2S microphone, the UDP inputs (WARLS, WLED audio sync), the analysis, the
# settings and API (docs/leds.md section 6.1). analysis.cpp and packets.cpp are pure C++, built by
# the host tests as well.
idf_component_register(SRCS "src/analysis.cpp" "src/audio.cpp" "src/api.cpp" "src/packets.cpp" "src/udp.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES effects
                       PRIV_REQUIRES esp_driver_i2s esp_http_server esp_timer freertos lwip platform webserver)
