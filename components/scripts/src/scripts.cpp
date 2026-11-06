// Scripts (px/scripts/scripts.hpp, docs/glow.md): the files, the slots, compiling, the programs for
// the render task. Nothing of a script is kept in RAM: its record in the index, its source and its
// program are files, read when asked for; the render task loads the program of the one it shows
// (load_program) and frees it when it goes.

#include "px/scripts/scripts.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "px/leds/leds.hpp"
#include "px/memory/large.hpp"
#include "px/platform/storage.hpp"

namespace px::scripts {

namespace glow = effects::glow;

namespace {

constexpr const char *kTag = "scripts";
constexpr const char *kDir = "/fs/scripts";
constexpr const char *kIndexPath = "/fs/scripts/index.bin";
constexpr const char *kIndexTemp = "/fs/scripts/index.tmp";
// the slots before the index (2026-11-05): a slug per line, read once
constexpr const char *kOldSlotsPath = "/fs/scripts/slots.txt";
constexpr size_t kSlots = leds::kScriptSlots;
constexpr size_t kRun = leds::kRunSlot;
// the compiler's task: the nesting it allows (16 blocks, parentheses and calls inside each other)
// takes the stack, 5500 bytes for the deepest scripts that compile (14 ifs, 14 parentheses, 7
// calls in calls; the ESP32 2026-11-05); compiler_stack_peak() tells what it used
constexpr uint32_t kCompileStack = 8192;
// the render task does not wait for a save that compiles: it tries again a second later. Not even
// a little: a task waiting for the lock lends its priority to the one that holds it, and the
// render task's is high (the compile task at the holder's own priority would get no time).
constexpr TickType_t kLoadWait = 0;

// A slot in the index (/fs/scripts/index.bin: IndexHead, then kSlots + 1 records, the run's last):
// what the lists show of it. slug[0] == 0: a free slot.
struct Record {
    char slug[kSlugSize];
    char name[glow::kNameSize];
    uint8_t ok;
    uint8_t reactive;
    uint8_t uses_color;
    uint8_t uses_palette;
    uint8_t param_count;
    uint8_t reserved[3];
    uint16_t error_line;
    uint16_t error_column;
    char error_text[sizeof(glow::Error::text)];
    uint32_t source_size;
    uint32_t program_size;
};

struct IndexHead {
    char magic[4];
    uint8_t version;
    uint8_t slots;
    uint16_t record_size;
};

constexpr IndexHead kIndexHead = {{'G', 'L', 'W', 'I'}, 1, static_cast<uint8_t>(kSlots + 1), sizeof(Record)};

// A program file (/fs/scripts/<slug>.bin, the run's run.bin): this head, then the program. It is
// used only by the firmware that compiled it, from the source it was compiled from; else the
// script is compiled again at start.
struct ProgramHead {
    char magic[4];
    // the first bytes of the firmware's ELF SHA-256
    uint32_t firmware;
    uint32_t source_crc;
    uint32_t size;
};

// recursive: each() calls back with it held, and the callback may ask for a script's params
SemaphoreHandle_t g_lock = nullptr;
SemaphoreHandle_t g_compiled = nullptr;
uint32_t g_revision = 0;
uint32_t g_stack_peak = 0;

void lock()
{
    xSemaphoreTakeRecursive(g_lock, portMAX_DELAY);
}

void unlock()
{
    xSemaphoreGiveRecursive(g_lock);
}

struct Job {
    std::string_view source;
    void *workspace;
    glow::Error *error;
    size_t size;
};

void set_error(glow::Error &error, const char *format, ...)
{
    error.line = 0;
    error.column = 0;
    va_list args;
    va_start(args, format);
    std::vsnprintf(error.text, sizeof(error.text), format, args);
    va_end(args);
}

uint32_t firmware_key()
{
    uint32_t key = 0;
    std::memcpy(&key, esp_app_get_description()->app_elf_sha256, sizeof(key));
    return key;
}

uint32_t crc_of(std::string_view source)
{
    return esp_rom_crc32_le(0, reinterpret_cast<const uint8_t *>(source.data()), static_cast<uint32_t>(source.size()));
}

// Done, it waits to be deleted by the caller: a task that deletes itself leaves its stack to the
// idle task, which does not get to run between compiles one after the other (a backup's scripts
// ran out of memory, 2026-11-05)
void compile_task(void *arg)
{
    auto *job = static_cast<Job *>(arg);
    job->size = glow::compile(job->source, job->workspace, *job->error);
    // ESP-IDF counts the stack in bytes
    const uint32_t used = kCompileStack - static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
    g_stack_peak = used > g_stack_peak ? used : g_stack_peak;
    xSemaphoreGive(g_compiled);
    vTaskSuspend(nullptr);
}

// `source` compiled: the workspace (large heap, the caller releases it) with the program at its
// start and its size, nullptr with `error` set. Under the lock.
void *compile(std::string_view source, glow::Error &error, size_t &size)
{
    size = 0;
    void *workspace = memory::allocate_large(glow::workspace_size());
    if (workspace == nullptr) {
        set_error(error, "no memory to compile (%u KB)", static_cast<unsigned>(glow::workspace_size() / 1024 + 1));
        return nullptr;
    }
    Job job{source, workspace, &error, 0};
    // at the caller's priority and on its core: it runs while the caller waits, and is suspended
    // (not running) when the caller deletes it, which frees its stack at once
    TaskHandle_t task = nullptr;
    if (xTaskCreatePinnedToCore(compile_task, "glow", kCompileStack, &job, uxTaskPriorityGet(nullptr), &task,
                                xPortGetCoreID()) != pdPASS) {
        memory::release_large(workspace);
        set_error(error, "no memory for the compiler's stack (%u KB)", static_cast<unsigned>(kCompileStack / 1024));
        return nullptr;
    }
    xSemaphoreTake(g_compiled, portMAX_DELAY);
    // a tick at a time, not a yield: one that only yields gives no time to a task of lower priority
    // (the compile task when this one borrowed a higher priority for the lock it holds) and starves
    // the idle task (the task watchdog, 2026-11-06)
    while (eTaskGetState(task) != eSuspended) {
        vTaskDelay(1);
    }
    vTaskDelete(task);
    if (job.size == 0) {
        memory::release_large(workspace);
        return nullptr;
    }
    size = job.size;
    return workspace;
}

void path_of(const char *slug, const char *extension, char (&out)[48])
{
    std::snprintf(out, sizeof(out), "%s/%s.%s", kDir, slug, extension);
}

// the source of `slug` on the large heap, NUL-terminated; nullptr when there is none (or too long)
char *read_source(const char *slug, size_t &length)
{
    char path[48];
    path_of(slug, "glow", path);
    FILE *file = std::fopen(path, "rb");
    if (file == nullptr) {
        return nullptr;
    }
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::rewind(file);
    char *text = size >= 0 && static_cast<size_t>(size) <= glow::kMaxSource
                     ? static_cast<char *>(memory::allocate_large(static_cast<size_t>(size) + 1))
                     : nullptr;
    if (text != nullptr) {
        length = std::fread(text, 1, static_cast<size_t>(size), file);
        text[length] = '\0';
    }
    std::fclose(file);
    return text;
}

// written next to it and renamed: a power cut keeps the old file or the new one
bool write_file(const char *path, const char *temp, const void *head, size_t head_size, const void *data, size_t size)
{
    FILE *file = std::fopen(temp, "wb");
    if (file == nullptr) {
        return false;
    }
    bool ok = head_size == 0 || std::fwrite(head, 1, head_size, file) == head_size;
    ok = ok && (size == 0 || std::fwrite(data, 1, size, file) == size);
    ok = std::fclose(file) == 0 && ok;
    if (ok) {
        std::remove(path);
        ok = std::rename(temp, path) == 0;
    }
    if (!ok) {
        std::remove(temp);
    }
    return ok;
}

bool write_source(const char *slug, std::string_view source)
{
    char path[48];
    char temp[48];
    path_of(slug, "glow", path);
    path_of(slug, "tmp", temp);
    return write_file(path, temp, nullptr, 0, source.data(), source.size());
}

bool write_program(const char *slug, uint32_t source_crc, const void *program, size_t size)
{
    char path[48];
    char temp[48];
    path_of(slug, "bin", path);
    path_of(slug, "btmp", temp);
    const ProgramHead head{{'G', 'L', 'W', 'P'}, firmware_key(), source_crc, static_cast<uint32_t>(size)};
    return write_file(path, temp, &head, sizeof(head), program, size);
}

void remove_files(const char *slug)
{
    char path[48];
    path_of(slug, "glow", path);
    std::remove(path);
    path_of(slug, "bin", path);
    std::remove(path);
}

// The head of the program file of `slug`; false when there is none
bool read_program_head(const char *slug, ProgramHead &head)
{
    char path[48];
    path_of(slug, "bin", path);
    FILE *file = std::fopen(path, "rb");
    if (file == nullptr) {
        return false;
    }
    const bool ok = std::fread(&head, sizeof(head), 1, file) == 1 && std::memcmp(head.magic, "GLWP", 4) == 0;
    std::fclose(file);
    return ok;
}

// The program of `slug` from its file on the large heap, checked; nullptr when there is none or it
// is not this firmware's
const glow::Program *read_program(const char *slug)
{
    char path[48];
    path_of(slug, "bin", path);
    FILE *file = std::fopen(path, "rb");
    if (file == nullptr) {
        return nullptr;
    }
    ProgramHead head{};
    void *bytes = nullptr;
    if (std::fread(&head, sizeof(head), 1, file) == 1 && std::memcmp(head.magic, "GLWP", 4) == 0 &&
        head.firmware == firmware_key() && head.size != 0 && head.size <= glow::workspace_size()) {
        bytes = memory::allocate_large(head.size);
        if (bytes != nullptr && std::fread(bytes, 1, head.size, file) != head.size) {
            memory::release_large(bytes);
            bytes = nullptr;
        }
    }
    std::fclose(file);
    const glow::Program *program = bytes != nullptr ? glow::program(bytes, head.size) : nullptr;
    if (bytes != nullptr && program == nullptr) {
        ESP_LOGW(kTag, "%s.bin does not check", slug);
        memory::release_large(bytes);
    }
    return program;
}

// ------------------------------------------------------------------------------------- the index

const char *slug_of(size_t slot, const Record &r)
{
    return slot == kRun ? kRunSlug : r.slug;
}

bool write_empty_index()
{
    FILE *file = std::fopen(kIndexTemp, "wb");
    if (file == nullptr) {
        return false;
    }
    bool ok = std::fwrite(&kIndexHead, sizeof(kIndexHead), 1, file) == 1;
    const Record empty{};
    for (size_t slot = 0; slot <= kRun && ok; slot++) {
        ok = std::fwrite(&empty, sizeof(empty), 1, file) == 1;
    }
    ok = std::fclose(file) == 0 && ok;
    if (ok) {
        std::remove(kIndexPath);
        ok = std::rename(kIndexTemp, kIndexPath) == 0;
    }
    return ok;
}

// The index opened at its first record (for reading, or "r+b"); nullptr when it is not there or not
// one of this firmware's layout
FILE *open_index(const char *mode)
{
    FILE *file = std::fopen(kIndexPath, mode);
    if (file == nullptr) {
        return nullptr;
    }
    IndexHead head{};
    if (std::fread(&head, sizeof(head), 1, file) != 1 || std::memcmp(&head, &kIndexHead, sizeof(head)) != 0) {
        std::fclose(file);
        return nullptr;
    }
    return file;
}

bool read_record(size_t slot, Record &out)
{
    out = {};
    FILE *file = open_index("rb");
    if (file == nullptr) {
        return false;
    }
    const bool ok = std::fseek(file, static_cast<long>(sizeof(IndexHead) + slot * sizeof(Record)), SEEK_SET) == 0 &&
                    std::fread(&out, sizeof(out), 1, file) == 1;
    std::fclose(file);
    out.slug[kSlugSize - 1] = '\0';
    out.name[glow::kNameSize - 1] = '\0';
    out.error_text[sizeof(out.error_text) - 1] = '\0';
    return ok;
}

bool write_record(size_t slot, const Record &record)
{
    FILE *file = open_index("r+b");
    if (file == nullptr) {
        if (!write_empty_index() || (file = open_index("r+b")) == nullptr) {
            ESP_LOGW(kTag, "the index could not be written");
            return false;
        }
    }
    bool ok = std::fseek(file, static_cast<long>(sizeof(IndexHead) + slot * sizeof(Record)), SEEK_SET) == 0 &&
              std::fwrite(&record, sizeof(record), 1, file) == 1;
    ok = std::fclose(file) == 0 && ok;
    if (!ok) {
        ESP_LOGW(kTag, "the index could not be written");
    }
    return ok;
}

// `each(slot, record)` for every slot of the index (the run's last) until it says false
template <class Each>
void each_record(Each each)
{
    FILE *file = open_index("rb");
    if (file == nullptr) {
        return;
    }
    Record r;
    for (size_t slot = 0; slot <= kRun && std::fread(&r, sizeof(r), 1, file) == 1; slot++) {
        r.slug[kSlugSize - 1] = '\0';
        r.name[glow::kNameSize - 1] = '\0';
        r.error_text[sizeof(r.error_text) - 1] = '\0';
        if (!each(slot, r)) {
            break;
        }
    }
    std::fclose(file);
}

int slot_of(const char *slug)
{
    int out = -1;
    each_record([&](size_t slot, const Record &r) {
        if (slot < kSlots && r.slug[0] != '\0' && std::strcmp(r.slug, slug) == 0) {
            out = static_cast<int>(slot);
        }
        return out < 0;
    });
    return out;
}

int free_slot()
{
    int out = -1;
    each_record([&](size_t slot, const Record &r) {
        if (slot < kSlots && r.slug[0] == '\0') {
            out = static_cast<int>(slot);
        }
        return out < 0;
    });
    return out;
}

// another animation's name: a built-in one or another script's that runs
bool name_taken(const char *name, size_t slot)
{
    for (size_t i = 0; i < effects::count(); i++) {
        if (std::strcmp(effects::info(i)->name, name) == 0) {
            return true;
        }
    }
    bool taken = false;
    each_record([&](size_t s, const Record &r) {
        taken = s != slot && s < kSlots && r.slug[0] != '\0' && r.program_size != 0 && std::strcmp(r.name, name) == 0;
        return !taken;
    });
    return taken;
}

// The slots with a program and the ones of them that need sound, for the render task
void tell_slots()
{
    uint32_t present = 0;
    uint32_t needs_sound = 0;
    each_record([&](size_t slot, const Record &r) {
        if ((r.slug[0] != '\0' || slot == kRun) && r.program_size != 0) {
            present |= 1u << slot;
            needs_sound |= r.reactive == static_cast<uint8_t>(effects::Reactive::Required) ? 1u << slot : 0u;
        }
        return true;
    });
    leds::set_script_slots(present, needs_sound);
}

bool same_params(const glow::Info &a, const glow::Info &b)
{
    if (a.param_count != b.param_count) {
        return false;
    }
    for (size_t k = 0; k < a.param_count; k++) {
        const glow::Param &p = a.params[k];
        const glow::Param &q = b.params[k];
        if (std::strcmp(p.name, q.name) != 0 || p.min != q.min || p.max != q.max || p.step != q.step ||
            p.initial != q.initial) {
            return false;
        }
    }
    return true;
}

void fill_record(Record &r, const glow::Info &info, size_t program_size)
{
    std::memcpy(r.name, info.name, sizeof(r.name));
    r.ok = 1;
    r.reactive = static_cast<uint8_t>(info.reactive);
    r.uses_color = info.uses_color ? 1 : 0;
    r.uses_palette = info.uses_palette ? 1 : 0;
    r.param_count = info.param_count;
    r.error_line = 0;
    r.error_column = 0;
    r.error_text[0] = '\0';
    r.program_size = static_cast<uint32_t>(program_size);
}

void fail_record(Record &r, const char *slug, const glow::Error &error)
{
    r.ok = 0;
    r.error_line = error.line;
    r.error_column = error.column;
    std::snprintf(r.error_text, sizeof(r.error_text), "%s", error.text);
    if (r.program_size == 0) {
        std::snprintf(r.name, sizeof(r.name), "%s", slug);
    }
}

// What a compile into a slot did, for the render task after the lock
struct Built {
    bool compiled = false;
    // the sliders start again (a new script, other params; the caller decides): at their defaults
    bool fresh = false;
    uint8_t sliders[glow::kMaxParams] = {};
};

// Compiles `source` into `slot` (its source file written before): when it compiles the program's
// file is written and the record says what it is, else the record has the error and the program
// before (if any) stays. Under the lock.
Built build(size_t slot, const char *slug, std::string_view source, glow::Error &error)
{
    Built out;
    Record r;
    read_record(slot, r);
    std::snprintf(r.slug, sizeof(r.slug), "%s", slot == kRun ? "" : slug);
    r.source_size = static_cast<uint32_t>(source.size());
    size_t size = 0;
    void *workspace = compile(source, error, size);
    const glow::Program *program = workspace != nullptr ? glow::program(workspace, size) : nullptr;
    if (workspace != nullptr && program == nullptr) {
        set_error(error, "the compiler made a program that does not check: please report the script");
    } else if (program != nullptr && slot < kSlots && name_taken(glow::info(*program).name, slot)) {
        set_error(error, "the name \"%s\" is another animation's", glow::info(*program).name);
        program = nullptr;
    } else if (program != nullptr && !write_program(slug_of(slot, r), crc_of(source), workspace, size)) {
        set_error(error, "the program's file could not be written");
        program = nullptr;
    }
    if (program == nullptr) {
        fail_record(r, slug_of(slot, r), error);
        write_record(slot, r);
        memory::release_large(workspace);
        return out;
    }
    const glow::Info &info = glow::info(*program);
    out.compiled = true;
    for (size_t k = 0; k < info.param_count; k++) {
        out.sliders[k] = glow::param_slider(info.params[k], info.params[k].initial);
    }
    fill_record(r, info, size);
    write_record(slot, r);
    memory::release_large(workspace);
    return out;
}

} // namespace

bool valid_slug(const char *slug)
{
    const size_t n = std::strlen(slug);
    // not a built-in animation's: the API and Home Assistant find those first
    if (n == 0 || n >= kSlugSize || std::strcmp(slug, kRunSlug) == 0 || effects::find(slug) < effects::count()) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        const char c = slug[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return true;
}

esp_err_t start()
{
    ESP_RETURN_ON_FALSE(g_lock == nullptr, ESP_ERR_INVALID_STATE, kTag, "already started");
    g_lock = xSemaphoreCreateRecursiveMutex();
    g_compiled = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(g_lock != nullptr && g_compiled != nullptr, ESP_ERR_NO_MEM, kTag, "lock");
    if (platform::storage_status().fs != platform::FsState::Mounted) {
        ESP_LOGW(kTag, "no file system: no scripts");
        return ESP_OK;
    }
    mkdir(kDir, 0775);
    lock();
    // the index; before it the slots were a slug per line
    if (FILE *file = open_index("rb")) {
        std::fclose(file);
    } else {
        write_empty_index();
        if (FILE *old = std::fopen(kOldSlotsPath, "r")) {
            char line[64];
            for (size_t slot = 0; slot < kSlots && std::fgets(line, sizeof(line), old) != nullptr; slot++) {
                line[std::strcspn(line, "\r\n")] = '\0';
                if (valid_slug(line)) {
                    // shorter than kSlugSize (valid_slug)
                    Record r{};
                    std::memcpy(r.slug, line, std::strlen(line));
                    std::memcpy(r.name, line, std::strlen(line));
                    write_record(slot, r);
                }
            }
            std::fclose(old);
            std::remove(kOldSlotsPath);
        }
        ESP_LOGI(kTag, "the index made anew");
    }
    // a script put there without a slot (a file system image, an unpacked backup) takes a free one
    uint32_t added = 0;
    if (DIR *dir = opendir(kDir)) {
        while (const dirent *e = readdir(dir)) {
            char slug[kSlugSize] = {};
            const char *dot = std::strrchr(e->d_name, '.');
            if (dot == nullptr || std::strcmp(dot, ".glow") != 0 || static_cast<size_t>(dot - e->d_name) >= kSlugSize) {
                continue;
            }
            std::memcpy(slug, e->d_name, static_cast<size_t>(dot - e->d_name));
            if (!valid_slug(slug)) {
                ESP_LOGW(kTag, "%s: not a script's name (a-z, 0-9 and -, not a built-in animation's)", e->d_name);
                continue;
            }
            if (slot_of(slug) >= 0) {
                continue;
            }
            const int slot = free_slot();
            if (slot < 0) {
                ESP_LOGW(kTag, "%s: no free slot", e->d_name);
                continue;
            }
            Record r{};
            std::snprintf(r.slug, sizeof(r.slug), "%s", slug);
            std::snprintf(r.name, sizeof(r.name), "%s", slug);
            write_record(static_cast<size_t>(slot), r);
            added |= 1u << slot;
        }
        closedir(dir);
    }
    // each script's program: the file of this firmware from this source, else compiled again; one
    // found without a slot (above) starts with its sliders at their defaults
    uint32_t compiled = 0;
    uint32_t fresh = 0;
    uint8_t defaults[kSlots][glow::kMaxParams] = {};
    size_t count = 0;
    const uint32_t firmware = firmware_key();
    for (size_t slot = 0; slot < kSlots; slot++) {
        Record r;
        if (!read_record(slot, r) || r.slug[0] == '\0') {
            continue;
        }
        size_t length = 0;
        char *text = read_source(r.slug, length);
        if (text == nullptr) {
            ESP_LOGW(kTag, "%s: no such script", r.slug);
            remove_files(r.slug);
            write_record(slot, Record{});
            continue;
        }
        count++;
        ProgramHead head{};
        const bool program_there = read_program_head(r.slug, head) && head.firmware == firmware;
        if (program_there && head.source_crc == crc_of({text, length}) && r.program_size == head.size) {
            memory::release_large(text);
            continue;
        }
        glow::Error error;
        const Built built = build(slot, r.slug, {text, length}, error);
        memory::release_large(text);
        if (built.compiled && ((added >> slot) & 1u) != 0) {
            fresh |= 1u << slot;
            std::memcpy(defaults[slot], built.sliders, sizeof(built.sliders));
        }
        if (!built.compiled) {
            ESP_LOGW(kTag, "%s: line %u: %s", r.slug, error.line, error.text);
            if (!program_there) {
                // the program of another firmware does not run here
                char path[48];
                path_of(r.slug, "bin", path);
                std::remove(path);
                read_record(slot, r);
                r.program_size = 0;
                write_record(slot, r);
            }
        }
        compiled |= 1u << slot;
    }
    // the editor's run is not kept over a restart
    write_record(kRun, Record{});
    char path[48];
    path_of(kRunSlug, "bin", path);
    std::remove(path);
    tell_slots();
    unlock();
    for (size_t slot = 0; slot < kSlots; slot++) {
        if ((fresh >> slot) & 1u) {
            leds::reset_script_style(slot, defaults[slot]);
        }
        if ((compiled >> slot) & 1u) {
            leds::script_changed(slot);
        }
    }
    ESP_LOGI(kTag, "%u scripts, %u compiled again; the compiler took %u bytes of stack at most",
             static_cast<unsigned>(count), static_cast<unsigned>(__builtin_popcount(compiled)),
             static_cast<unsigned>(g_stack_peak));
    return ESP_OK;
}

bool running()
{
    return g_lock != nullptr;
}

namespace {

// a slot of the index that holds a script (the run's: one that was run)
bool there(size_t slot, const Record &r)
{
    return r.slug[0] != '\0' || (slot == kRun && r.source_size != 0);
}

void to_script(size_t slot, const Record &r, Script &out)
{
    out = {};
    std::snprintf(out.slug, sizeof(out.slug), "%s", slug_of(slot, r));
    std::memcpy(out.name, r.name, sizeof(out.name));
    out.ok = r.ok != 0;
    out.error.line = r.error_line;
    out.error.column = r.error_column;
    std::memcpy(out.error.text, r.error_text, sizeof(out.error.text));
    out.source_size = r.source_size;
    out.program_size = r.program_size;
    out.reactive = static_cast<effects::Reactive>(r.reactive);
    out.uses_color = r.uses_color != 0;
    out.uses_palette = r.uses_palette != 0;
    out.param_count = r.param_count;
}

} // namespace

bool get(size_t slot, Script &out)
{
    if (g_lock == nullptr || slot > kRun) {
        return false;
    }
    lock();
    Record r;
    const bool found = read_record(slot, r) && there(slot, r);
    unlock();
    if (found) {
        to_script(slot, r, out);
    }
    return found;
}

void each(void (*each)(size_t slot, const Script &script, void *context), void *context)
{
    if (g_lock == nullptr) {
        return;
    }
    lock();
    Script s;
    each_record([&](size_t slot, const Record &r) {
        if (there(slot, r)) {
            to_script(slot, r, s);
            each(slot, s, context);
        }
        return true;
    });
    unlock();
}

bool params(size_t slot, glow::Info &out)
{
    if (g_lock == nullptr || slot > kRun) {
        return false;
    }
    lock();
    Record r;
    const glow::Program *program =
        read_record(slot, r) && r.program_size != 0 ? read_program(slug_of(slot, r)) : nullptr;
    unlock();
    if (program == nullptr) {
        return false;
    }
    out = glow::info(*program);
    memory::release_large(const_cast<glow::Program *>(program));
    return true;
}

static_assert(kSlugSize == leds::kScriptSlugSize);

const glow::Program *load_program(size_t slot, char (&slug)[kSlugSize])
{
    if (g_lock == nullptr || slot > kRun || xSemaphoreTakeRecursive(g_lock, kLoadWait) != pdTRUE) {
        return nullptr;
    }
    Record r;
    const glow::Program *program =
        read_record(slot, r) && r.program_size != 0 ? read_program(slug_of(slot, r)) : nullptr;
    unlock();
    if (program != nullptr) {
        std::snprintf(slug, kSlugSize, "%s", slug_of(slot, r));
    }
    return program;
}

uint32_t revision()
{
    if (g_lock == nullptr) {
        return 0;
    }
    lock();
    const uint32_t out = g_revision;
    unlock();
    return out;
}

int find(const char *slug_or_name)
{
    if (g_lock == nullptr) {
        return -1;
    }
    lock();
    int out = -1;
    const bool run = std::strcmp(slug_or_name, kRunSlug) == 0;
    each_record([&](size_t slot, const Record &r) {
        if (slot == kRun) {
            out = run && r.program_size != 0 ? leds::kRunScript : -1;
        } else if (!run && r.slug[0] != '\0' &&
                   (std::strcmp(r.slug, slug_or_name) == 0 || (r.program_size != 0 && std::strcmp(r.name, slug_or_name) == 0))) {
            out = leds::script_effect(slot);
        }
        return out < 0;
    });
    unlock();
    return out;
}

bool names(size_t effect, char (&slug)[kSlugSize], char (&name)[glow::kNameSize])
{
    const int slot = leds::script_slot(effect);
    if (g_lock == nullptr || slot < 0) {
        return false;
    }
    lock();
    Record r;
    const bool there = read_record(static_cast<size_t>(slot), r) && (r.slug[0] != '\0' || slot == static_cast<int>(kRun));
    unlock();
    if (there) {
        std::snprintf(slug, kSlugSize, "%s", slug_of(static_cast<size_t>(slot), r));
        std::snprintf(name, glow::kNameSize, "%s", r.name);
    }
    return there;
}

esp_err_t source(const char *slug, char *out, size_t size, size_t &length)
{
    if (g_lock == nullptr || !valid_slug(slug) || size == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    lock();
    size_t n = 0;
    char *text = read_source(slug, n);
    unlock();
    if (text == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }
    length = n < size - 1 ? n : size - 1;
    std::memcpy(out, text, length);
    out[length] = '\0';
    memory::release_large(text);
    return ESP_OK;
}

esp_err_t save(const char *slug, std::string_view source, glow::Error &error)
{
    error = {};
    if (g_lock == nullptr) {
        set_error(error, "the scripts did not start");
        return ESP_ERR_INVALID_STATE;
    }
    if (effects::find(slug) < effects::count()) {
        set_error(error, "\"%s\" is a built-in animation's name", slug);
        return ESP_ERR_INVALID_ARG;
    }
    if (!valid_slug(slug)) {
        set_error(error, "a script's file name: 1 to %u of a-z, 0-9 and -, not \"run\"",
                  static_cast<unsigned>(kSlugSize - 1));
        return ESP_ERR_INVALID_ARG;
    }
    if (source.size() > glow::kMaxSource) {
        set_error(error, "the script is longer than %u bytes", static_cast<unsigned>(glow::kMaxSource));
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    // the index (an open file takes a 4 KB cache): without it no slot is known
    FILE *probe = open_index("rb");
    if (probe == nullptr) {
        unlock();
        set_error(error, "the scripts' index could not be read (no memory for the file?)");
        return ESP_ERR_NO_MEM;
    }
    std::fclose(probe);
    int slot = slot_of(slug);
    const bool added = slot < 0;
    slot = added ? free_slot() : slot;
    esp_err_t err = ESP_OK;
    Built built;
    if (slot < 0) {
        set_error(error, "all %u places are taken: delete a script first", static_cast<unsigned>(kSlots));
        err = ESP_ERR_NO_MEM;
    } else if (!write_source(slug, source)) {
        set_error(error, "the file could not be written");
        err = ESP_FAIL;
    } else {
        // a new one starts with its sliders at their defaults, one saved again keeps them while its
        // params stay the same
        const glow::Program *before = added ? nullptr : read_program(slug);
        built = build(static_cast<size_t>(slot), slug, source, error);
        if (built.compiled && before != nullptr) {
            const glow::Program *now = read_program(slug);
            built.fresh = now == nullptr || !same_params(glow::info(*before), glow::info(*now));
            memory::release_large(const_cast<glow::Program *>(now));
        } else if (built.compiled) {
            built.fresh = true;
        }
        memory::release_large(const_cast<glow::Program *>(before));
        tell_slots();
        g_revision++;
        ESP_LOGI(kTag, "%s saved in slot %d%s", slug, slot, error.text[0] != '\0' ? ", with an error" : "");
    }
    unlock();
    if (built.compiled) {
        if (built.fresh) {
            leds::reset_script_style(static_cast<size_t>(slot), built.sliders);
        }
        leds::script_changed(static_cast<size_t>(slot));
    }
    return err;
}

esp_err_t check(std::string_view source, glow::Error &error)
{
    error = {};
    if (g_lock == nullptr) {
        set_error(error, "the scripts did not start");
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    size_t size = 0;
    void *workspace = compile(source, error, size);
    unlock();
    memory::release_large(workspace);
    return ESP_OK;
}

esp_err_t run(std::string_view source, glow::Error &error)
{
    error = {};
    if (g_lock == nullptr) {
        set_error(error, "the scripts did not start");
        return ESP_ERR_INVALID_STATE;
    }
    if (source.size() > glow::kMaxSource) {
        set_error(error, "the script is longer than %u bytes", static_cast<unsigned>(glow::kMaxSource));
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    const glow::Program *before = read_program(kRunSlug);
    Built built = build(kRun, kRunSlug, source, error);
    if (built.compiled && before != nullptr) {
        const glow::Program *now = read_program(kRunSlug);
        built.fresh = now == nullptr || !same_params(glow::info(*before), glow::info(*now));
        memory::release_large(const_cast<glow::Program *>(now));
    } else if (built.compiled) {
        built.fresh = true;
    }
    memory::release_large(const_cast<glow::Program *>(before));
    tell_slots();
    g_revision++;
    unlock();
    if (built.compiled) {
        if (built.fresh) {
            leds::reset_script_style(kRun, built.sliders);
        }
        leds::script_changed(kRun);
        leds::set_effect(leds::kRunScript);
    }
    return ESP_OK;
}

esp_err_t remove(const char *slug)
{
    if (g_lock == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    const int slot = slot_of(slug);
    if (slot < 0) {
        unlock();
        return ESP_ERR_NOT_FOUND;
    }
    remove_files(slug);
    write_record(static_cast<size_t>(slot), Record{});
    tell_slots();
    g_revision++;
    unlock();
    // the slot starts again: the default style and sliders; a light showing it goes to the first
    leds::reset_script_style(static_cast<size_t>(slot), nullptr);
    leds::script_changed(static_cast<size_t>(slot));
    ESP_LOGI(kTag, "%s deleted", slug);
    return ESP_OK;
}

uint32_t compiler_stack_peak()
{
    return g_stack_peak;
}

esp_err_t disassemble(const char *slug, void (*line)(const char *text, void *context), void *context)
{
    if (g_lock == nullptr || !valid_slug(slug)) {
        return ESP_ERR_NOT_FOUND;
    }
    lock();
    size_t length = 0;
    char *text = read_source(slug, length);
    glow::Error error;
    size_t size = 0;
    void *workspace = text != nullptr ? compile({text, length}, error, size) : nullptr;
    unlock();
    memory::release_large(text);
    if (text == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }
    if (workspace == nullptr) {
        char message[sizeof(error.text) + 32];
        std::snprintf(message, sizeof(message), "line %u, column %u: %s", error.line, error.column, error.text);
        line(message, context);
        return ESP_FAIL;
    }
    if (const glow::Program *p = glow::program(workspace, size)) {
        glow::disassemble(*p, line, context);
    }
    memory::release_large(workspace);
    return ESP_OK;
}

} // namespace px::scripts
