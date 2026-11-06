# The LEDs, the presets and the scripts (CONFIG_PX_SCRIPTS) in the web API (docs/ui/web/web-app.md
# section 4).
set(srcs "src/api_leds.cpp" "src/api_presets.cpp")
if(CONFIG_PX_SCRIPTS)
    list(APPEND srcs "src/api_scripts.cpp")
endif()
idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES effects esp_http_server esp_timer formats freertos leds memory output presets scripts
                                     webserver)
