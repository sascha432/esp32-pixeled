# The recovery app (docs/architecture.md section 9.5): built with PX_APP=recovery, flashed to the
# factory partition. Small: Wi-Fi, hotspot, web server and the update, no LEDs.
idf_component_register(SRCS "recovery.cpp"
                       PRIV_REQUIRES board esp_driver_gpio esp_event logbuf network ota platform webserver)
