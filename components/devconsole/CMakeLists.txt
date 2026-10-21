# Serial console for development (CONFIG_PX_DEV_CONSOLE).
idf_component_register(SRCS "src/devconsole.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES audio board console effects layout leds memory output esp_driver_uart esp_driver_usb_serial_jtag esp_system esp_timer light_sensor logbuf motion network platform relay webserver)
