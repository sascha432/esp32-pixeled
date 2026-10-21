# The layout of the LEDs: canvas, panels, the table canvas -> LED (docs/leds.md section 4.1).
# Pure C++, built by the host tests as well.
idf_component_register(SRCS "src/layout.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES memory)
