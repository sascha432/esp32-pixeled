# The LEDs in the web API (docs/ui/web/web-app.md section 4).
idf_component_register(SRCS "src/api_leds.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES effects esp_http_server formats freertos leds output webserver)
