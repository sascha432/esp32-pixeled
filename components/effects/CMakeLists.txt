# The animations (docs/leds.md section 6). Pure C++, built by the host tests as well. Glow, the
# scripts' compiler and machine (docs/glow.md), only with CONFIG_PX_SCRIPTS.
set(srcs "src/effects.cpp" "src/particles.cpp" "src/curves.cpp" "src/fields.cpp" "src/overlay.cpp")
if(CONFIG_PX_SCRIPTS)
    list(APPEND srcs "src/glow/assignments.cpp" "src/glow/calls.cpp" "src/glow/compiler.cpp" "src/glow/declarations.cpp"
                     "src/glow/expressions.cpp" "src/glow/lexer.cpp" "src/glow/registers.cpp" "src/glow/runtime.cpp"
                     "src/glow/statements.cpp" "src/glow/vm.cpp")
endif()
idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include")
if(CONFIG_PX_SCRIPTS)
    # the machine's switch of 130 instructions: a jump table, not a chain of compares (it runs from
    # flash, never with the cache off)
    set_source_files_properties("src/glow/vm.cpp" PROPERTIES COMPILE_OPTIONS "-fjump-tables;-ftree-switch-conversion")
endif()
