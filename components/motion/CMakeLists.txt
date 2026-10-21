# A motion sensor on a GPIO: auto off of the LEDs and MQTT (docs/architecture.md section 9.7b).
idf_component_register(SRCS "src/motion.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES esp_driver_gpio esp_timer)
