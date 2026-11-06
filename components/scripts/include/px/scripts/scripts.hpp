#pragma once

// Scripts (docs/glow.md, docs/leds.md section 6.5): animations in Glow, kept on LittleFS as
// /fs/scripts/<slug>.glow, at most CONFIG_PX_SCRIPT_SLOTS of them. Each one has a slot (a record
// per slot in /fs/scripts/index.bin: its slug, name, state and sizes); its animation number
// (leds::State::effect, the presets) is leds::kScriptBase + slot. A script is compiled when saved
// (and at start when its program's file is not this firmware's or not from its source), on a task
// of its own (the compiler's nesting needs the stack), into /fs/scripts/<slug>.bin. Nothing of a
// script is in RAM: the render task loads the program of the one it shows (load_program,
// leds::Start::load_script) and frees it when it goes; the lists read the index. The editor's Run
// compiles into the slot leds::kRunSlot (slug "run", run.bin) and shows it without saving. Only
// with CONFIG_PX_SCRIPTS.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "esp_err.h"
#include "px/effects/glow.hpp"

namespace px::scripts {

constexpr size_t kSlugSize = 24;
// the editor's run (leds::kRunSlot)
constexpr const char *kRunSlug = "run";

// A script as the lists show it (read from the index)
struct Script {
    char slug[kSlugSize];
    // its program's name; its slug until one compiled
    char name[effects::glow::kNameSize];
    // the source compiled; else `error` says why (line 0: not because of a line) and the program
    // before it (if any) runs on
    bool ok;
    effects::glow::Error error;
    uint32_t source_size;
    // 0: no program runs
    uint32_t program_size;
    // what its program uses; its params with params()
    effects::Reactive reactive;
    bool uses_color;
    bool uses_palette;
    uint8_t param_count;
};

esp_err_t start();
bool running();

// The script in `slot` (0 .. leds::kScriptSlots - 1, leds::kRunSlot) into `out`; false for an
// empty slot
bool get(size_t slot, Script &out);
// `each(slot, script, context)` for every script there, the run's last; the index is read once
void each(void (*each)(size_t slot, const Script &script, void *context), void *context);
// The info of the program in `slot` (its name and params, read from its file); false without one
bool params(size_t slot, effects::glow::Info &out);
// The program in `slot` on the large heap for the render task (leds::Start::load_script), and its
// slug: the caller frees it (memory::release_large); nullptr without one, or while a save compiles
const effects::glow::Program *load_program(size_t slot, char (&slug)[kSlugSize]);
// changes with every save, delete and run (the lists, Home Assistant's effects)
uint32_t revision();
// The animation number of the script with this slug or name (not the run's: "run"), -1 for none
int find(const char *slug_or_name);
// The slug and the name of animation number `effect` when it is a script there
bool names(size_t effect, char (&slug)[kSlugSize], char (&name)[effects::glow::kNameSize]);
// 1 to 23 of a-z, 0-9 and '-', not "run" and not a built-in animation's slug
bool valid_slug(const char *slug);

// The source of `slug` into `out` (`size` bytes, NUL-terminated); ESP_ERR_NOT_FOUND
esp_err_t source(const char *slug, char *out, size_t size, size_t &length);
// Saves `source` as `slug` (a new one takes a free slot) and compiles it. The file is written in any
// case (an explicit save); the program is replaced only when it compiles (`error.text` empty then).
// ESP_ERR_INVALID_ARG: a bad slug or too long; ESP_ERR_NO_MEM: no free slot or no memory; ESP_FAIL:
// the file could not be written. The reason in `error` for all of them.
esp_err_t save(const char *slug, std::string_view source, effects::glow::Error &error);
// Compiles `source` without keeping it: `error.text` empty when it compiles (the backup's check)
esp_err_t check(std::string_view source, effects::glow::Error &error);
// Compiles `source` into the editor's run slot and shows it, without saving
esp_err_t run(std::string_view source, effects::glow::Error &error);
esp_err_t remove(const char *slug);
// the most stack a compile took (the console's `script`)
uint32_t compiler_stack_peak();
// The program of `slug` as text, a line at a time (compiled again; the console's `script dis`)
esp_err_t disassemble(const char *slug, void (*line)(const char *text, void *context), void *context);

} // namespace px::scripts
