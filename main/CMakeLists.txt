# The PixeLED firmware: composition root only (docs/architecture.md section 3).
idf_component_register(SRCS "backup.cpp" "main.cpp"
                       PRIV_REQUIRES app_update audio board config devconsole effects esp_app_format output esp_event esp_timer heap
                                     layout leds leds_api light_sensor logbuf memory hass motion network platform relay webserver)
