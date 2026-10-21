# Board support (docs/architecture.md section 8): one file per board in boards/, compiled only
# when its Kconfig option is selected. The recovery app uses it too (the board tag, the button).
set(srcs "src/board.cpp")
if(CONFIG_PX_BOARD_ESP32S3_DEVKIT)
    list(APPEND srcs "boards/esp32s3_devkit.cpp")
endif()
if(CONFIG_PX_BOARD_ESP32_DEVKIT)
    list(APPEND srcs "boards/esp32_devkit.cpp")
endif()
if(CONFIG_PX_BOARD_ESP32C3_DEVKIT)
    list(APPEND srcs "boards/esp32c3_devkit.cpp")
endif()
if(CONFIG_PX_BOARD_ESP32C6_DEVKIT)
    list(APPEND srcs "boards/esp32c6_devkit.cpp")
endif()

# The variant (px_board_variant.hpp, included by every board file): the one of
# boards/<board>/variants/<name>/ when tools/build.py passes its directory, else src/variant,
# which changes nothing. Only one of the two is on the include path.
if(PX_VARIANT_DIR)
    if(NOT EXISTS "${PX_VARIANT_DIR}/px_board_variant.hpp")
        message(FATAL_ERROR "the variant directory ${PX_VARIANT_DIR} has no px_board_variant.hpp "
                            "(copy components/board/src/variant/px_board_variant.hpp)")
    endif()
    set(variant_dir "${PX_VARIANT_DIR}")
else()
    set(variant_dir "src/variant")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "src" "${variant_dir}"
                       REQUIRES esp_driver_i2c)

# the board tag is never referenced by code: keep it in the image (docs: app_image_format)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u px_image_tag")
