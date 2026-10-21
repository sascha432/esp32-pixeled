# GPIO relays: the LED power supply (docs/architecture.md section 9.7a).
idf_component_register(SRCS "src/relay.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES esp_driver_gpio esp_hal_gpio)
# relay.cpp has the wrapper that switches the relays of off_at_crash() off in the panic handler
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")
