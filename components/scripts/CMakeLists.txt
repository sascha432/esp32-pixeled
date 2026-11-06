# Glow scripts: the files on LittleFS, compiling them, the programs for the render task (docs/glow.md).
# Without CONFIG_PX_SCRIPTS nothing is compiled: the component is only its option.
set(srcs)
if(CONFIG_PX_SCRIPTS)
    set(srcs "src/scripts.cpp")
endif()
idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       REQUIRES effects
                       PRIV_REQUIRES esp_app_format freertos leds memory platform)
