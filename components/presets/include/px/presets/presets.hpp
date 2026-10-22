#pragma once

// Presets (docs/leds.md section 10): the light as it was saved under a name, the animation with its
// color, speed, brightness and style (rotation, sound, palette). Up to kMaxPresets, in the order
// they were saved, stored on LittleFS (/fs/presets.bin) at once when they change (an explicit
// action). The list is kept in RAM under a lock; applying one posts the values to the render task.

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "px/effects/effects.hpp"
#include "px/leds/leds.hpp"

namespace px::presets {

constexpr size_t kMaxPresets = 16;
constexpr size_t kNameSize = 32;

struct Preset {
    // UTF-8, not empty
    char name[kNameSize];
    // an index of px::effects (a new animation goes before the test patterns: the index stays)
    uint8_t effect;
    effects::Rgb color;
    uint8_t speed;
    uint8_t brightness;
    leds::Style style;
};

// Reads the stored presets. Call once, after the file system is mounted; without the file, none.
esp_err_t start();

size_t count();
// false for an index from count() on
bool get(size_t index, Preset &out);
// the index of the preset named `name`, count() when there is none
size_t find(const char *name);

// The light as it is now under `name`: replaces the preset of that name, else added at the end.
// false with the problem in `error` (no name, the list is full, the file could not be written).
bool save_current(const char *name, char *error, size_t error_size);
bool remove(size_t index, char *error, size_t error_size);
// Replaces every preset (the backup); each is checked: false with the problem in `error`, and
// nothing changed.
bool set_all(const Preset *presets, size_t count, char *error, size_t error_size);

// Shows preset `index`: its animation, style, color, speed and brightness, and switches the LEDs on.
// false for an index that is not there.
bool apply(size_t index);
// The preset after the one shown last (or the one that matches the light), the first after the
// last; false without presets.
bool apply_next();
// the preset the light matches now (animation, color, speed, brightness, style), count() when none
size_t current();
// counts every change of the list (Home Assistant announces the names again)
uint32_t revision();

} // namespace px::presets
