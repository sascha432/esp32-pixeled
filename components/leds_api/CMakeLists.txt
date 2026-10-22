# The LEDs and the presets in the web API (docs/ui/web/web-app.md section 4).
idf_component_register(SRCS "src/api_leds.cpp" "src/api_presets.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES effects esp_http_server esp_timer formats freertos leds memory output presets webserver)
