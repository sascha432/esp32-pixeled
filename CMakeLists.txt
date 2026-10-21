# esp32-pixeled (PixeLED): one ESP-IDF project, two applications (docs/architecture.md section 9.5).
#
#   PX_APP=main      the LED firmware, top-level component "main", runs from an OTA partition
#   PX_APP=recovery  the recovery app, component "recovery", runs from the factory partition
#
# Both share every component in components/, the board configs and the partition tables.
# tools/build.py sets PX_APP, the board layers and one build directory per board and app.
cmake_minimum_required(VERSION 3.22)

if(NOT DEFINED PX_APP)
    set(PX_APP main)
endif()

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/recovery")

# COMPONENTS limits the build to the application's top-level component and what it requires, so
# the recovery app does not compile the LED engine or other components it never links.
if(PX_APP STREQUAL "main")
    set(COMPONENTS main)
    set(EXCLUDE_COMPONENTS recovery)
    set(PX_PROJECT_NAME pixeled)
elseif(PX_APP STREQUAL "recovery")
    set(COMPONENTS recovery)
    set(EXCLUDE_COMPONENTS main)
    set(PX_PROJECT_NAME pixeled-recovery)
else()
    message(FATAL_ERROR "PX_APP must be 'main' or 'recovery', not '${PX_APP}'")
endif()

set(PROJECT_VER "0.1.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(${PX_PROJECT_NAME})

# The FPUs of the ESP32 and S3 are single precision (the C3 and C6 have none); double math runs in
# software. A float that becomes a double without a cast is an error in our own components (printf
# arguments take the cast).
idf_build_get_property(px_components BUILD_COMPONENTS)
string(TOLOWER "${CMAKE_CURRENT_LIST_DIR}" px_root)
foreach(name IN LISTS px_components)
    idf_component_get_property(dir ${name} COMPONENT_DIR)
    idf_component_get_property(lib ${name} COMPONENT_LIB)
    string(TOLOWER "${dir}" dir)
    get_target_property(type ${lib} TYPE)
    if(type STREQUAL "INTERFACE_LIBRARY")
        continue()
    endif()
    if(dir MATCHES "^${px_root}/(components|main|recovery)(/|$)")
        target_compile_options(${lib} PRIVATE -Werror=double-promotion)
    endif()
endforeach()
