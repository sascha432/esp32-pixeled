# Home Assistant over MQTT: the connection and the discovery (docs/leds.md section 8), main
# firmware only (component "hass": "mqtt" is the name of the client, espressif/mqtt). The client and
# cJSON come from idf_component.yml.
idf_component_register(SRCS "src/mqtt.cpp" "src/api.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES effects esp_http_server esp_timer formats freertos leds platform webserver)
