# The animations (docs/leds.md section 6). Pure C++, built by the host tests as well.
idf_component_register(SRCS "src/effects.cpp" "src/overlay.cpp"
                       INCLUDE_DIRS "include")
