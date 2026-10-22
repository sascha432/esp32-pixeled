# Serial console for development (CONFIG_PX_DEV_CONSOLE).
idf_component_register(SRCS "src/devconsole.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES audio board console effects hass input layout leds memory output presets esp_driver_uart esp_driver_usb_serial_jtag esp_system esp_timer light_sensor logbuf motion network platform relay webserver)
