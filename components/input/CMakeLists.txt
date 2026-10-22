# Buttons and a rotary encoder: their actions, settings and API (docs/leds.md section 9).
# button.cpp is pure C++, built by the host tests as well.
idf_component_register(SRCS "src/api.cpp" "src/button.cpp" "src/input.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES esp_driver_gpio esp_driver_pcnt esp_http_server esp_timer freertos platform webserver)
