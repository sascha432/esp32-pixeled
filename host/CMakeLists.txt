# Host build: the pure components compiled for the PC and tested there (docs/architecture.md
# section 2). Plain CMake, not the ESP-IDF build system; tools/host_test.py runs it with MSVC.
cmake_minimum_required(VERSION 3.22)
project(pixeled_host LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(PX_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

if(MSVC)
    # /utf-8: the sources are UTF-8 (°C); /permissive-: standard C++; _CRT_SECURE_NO_WARNINGS:
    # the code uses snprintf and strncpy deliberately, not the _s variants
    add_compile_options(/permissive- /utf-8 /EHsc)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    set(PX_WARNINGS /W4)
else()
    set(PX_WARNINGS -Wall -Wextra)
endif()

add_executable(px_tests
    tests/backup_test.cpp
    tests/check.cpp
    tests/effects_test.cpp
    tests/frame_test.cpp
    tests/layout_test.cpp
    tests/multipart_test.cpp
    tests/network_test.cpp
    tests/timezone_test.cpp
    tests/yaml_test.cpp
    ${PX_ROOT}/components/config/src/backup.cpp
    ${PX_ROOT}/components/effects/src/effects.cpp
    ${PX_ROOT}/components/formats/src/json_writer.cpp
    ${PX_ROOT}/components/layout/src/layout.cpp
    ${PX_ROOT}/components/leds/src/frame.cpp
    ${PX_ROOT}/components/formats/src/multipart.cpp
    ${PX_ROOT}/components/formats/src/yaml.cpp
    ${PX_ROOT}/components/memory/src/large.cpp
    ${PX_ROOT}/components/network/src/merge.cpp
    ${PX_ROOT}/components/network/src/selection.cpp
    ${PX_ROOT}/components/platform/src/timezone.cpp
)
target_include_directories(px_tests PRIVATE
    tests
    ${PX_ROOT}/components/config/include
    ${PX_ROOT}/components/effects/include
    ${PX_ROOT}/components/formats/include
    ${PX_ROOT}/components/layout/include
    ${PX_ROOT}/components/leds/src
    ${PX_ROOT}/components/memory/include
    ${PX_ROOT}/components/network/include
    ${PX_ROOT}/components/network/src
    ${PX_ROOT}/components/platform/include
    # esp_err.h for the network header
    tests/stubs
)
target_compile_options(px_tests PRIVATE ${PX_WARNINGS})
# the sample files of the tests
target_compile_definitions(px_tests PRIVATE PX_SOURCE_ROOT="${PX_ROOT}")
