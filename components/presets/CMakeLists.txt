# Presets: the light saved under a name, on LittleFS (docs/leds.md section 10).
idf_component_register(SRCS "src/presets.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES effects leds
                       PRIV_REQUIRES freertos platform)
