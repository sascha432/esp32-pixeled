#pragma once

// PX_PSRAM_BSS puts a static object into PSRAM instead of the internal .bss (docs/architecture.md
// section 11), in code that the PC builds compile too; firmware-only code writes EXT_RAM_BSS_ATTR
// itself. Without CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY (every board but the S3, and the
// recovery app) and on the PC it is nothing.
//
// The section is never loaded from the image: ESP-IDF zeroes it before the C++ constructors run.
// Only an object that starts as zeros, or whose values are written at startup, may live there. One
// that the compiler would fill in at build time with non-zero values starts as zeros.

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define PX_PSRAM_BSS EXT_RAM_BSS_ATTR
#else
#define PX_PSRAM_BSS
#endif
