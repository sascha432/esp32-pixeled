#include "px/devconsole/devconsole.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unistd.h>

#include "sdkconfig.h"
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#else
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#endif
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "px/audio/audio.hpp"
#include "px/board/board.hpp"
#include "px/light_sensor/ldr.hpp"
#include "px/logbuf/logbuf.hpp"
#include "px/light_sensor/light_sensor.hpp"
#include "px/network/network.hpp"
#include "px/platform/settings.hpp"
#include "px/platform/storage.hpp"
#include "px/motion/motion.hpp"
#include "px/layout/layout.hpp"
#include "px/leds/leds.hpp"
#include "px/output/output.hpp"
#include "px/relay/relay.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "px/webserver/webserver.hpp"
#include "linenoise/linenoise.h"

#if CONFIG_LIBC_PICOLIBC
// linenoise reads and writes the stdio streams of its task through these (esp_console's own REPL
// task sets them the same way, console_stdio_private.h)
#if !CONFIG_LIBC_PICOLIBC_NEWLIB_COMPATIBILITY
#define tls_stdin linenoise_stdin
#define tls_stdout linenoise_stdout
#endif
extern "C" __thread FILE *tls_stdin;
extern "C" __thread FILE *tls_stdout;
#endif

namespace px::devconsole {

#if CONFIG_PX_DEV_CONSOLE

namespace {

namespace net = px::network;

int usage(const char *text)
{
    std::printf("usage: %s\n", text);
    return 1;
}

int check(esp_err_t err)
{
    if (err != ESP_OK) {
        std::printf("error: %s\n", esp_err_to_name(err));
        return 1;
    }
    return 0;
}

void print_status()
{
    const net::Status s = net::status();
    std::printf("wifi:    %s", net::state_name(s.state));
    if (s.ssid[0] != 0) {
        std::printf(", '%s'", s.ssid);
    }
    if (s.state == net::StaState::Connected) {
        std::printf(", %s, %d dBm, channel %u, connected in %lu ms (%s)", s.ip, s.rssi, s.channel,
                    static_cast<unsigned long>(s.connect_ms), s.connect_fast ? "fast" : "with scan");
    }
    std::printf("\n");
    if (s.error[0] != 0) {
        std::printf("error:   %s\n", s.error);
    }
    std::printf("policy:  %s\n", s.policy == net::Policy::EnteredOrder ? "entered order" : "strongest signal");
    std::printf("hotspot: %s, '%s', %s %s, %u clients", s.hotspot_on ? "on" : "off", s.hotspot_ssid,
                s.hotspot_open ? "open for the setup, then password" : "password", s.hotspot_password,
                s.hotspot_clients);
    if (s.hotspot_off_in_s != 0) {
        std::printf(", off in %lu s", static_cast<unsigned long>(s.hotspot_off_in_s));
    }
    std::printf("%s\n", s.hotspot_enabled ? ", kept on" : "");
}

void print_saved()
{
    net::SavedNetworkInfo saved[net::kMaxNetworks];
    const size_t count = net::saved_networks(saved, net::kMaxNetworks);
    if (count == 0) {
        std::printf("no saved networks\n");
    }
    for (size_t i = 0; i < count; i++) {
        std::printf("%u  %s%s\n", static_cast<unsigned>(i + 1), saved[i].ssid,
                    saved[i].password_needed ? "  (password needed)" : saved[i].open ? "  (open)" : "");
    }
}

void print_scan()
{
    net::ScanEntry entries[20];
    const size_t count = net::last_scan(entries, 20);
    if (count == 0) {
        std::printf("no scan results, run 'wifi scan' first\n");
    }
    for (size_t i = 0; i < count; i++) {
        std::printf("%4d dBm  ch %2u  %s %s%s\n", entries[i].rssi, entries[i].channel, entries[i].secure ? "lock" : "open",
                    entries[i].ssid, entries[i].saved ? "  (saved)" : "");
    }
}

// a 1-based list position from the command line
bool position(const char *text, uint8_t &index)
{
    const int value = std::atoi(text);
    if (value < 1 || value > static_cast<int>(net::kMaxNetworks)) {
        return false;
    }
    index = static_cast<uint8_t>(value - 1);
    return true;
}

int cmd_wifi(int argc, char **argv)
{
    if (argc < 2 || std::strcmp(argv[1], "status") == 0) {
        print_status();
        return 0;
    }
    const char *sub = argv[1];
    uint8_t index = 0;
    if (std::strcmp(sub, "list") == 0) {
        print_saved();
        return 0;
    }
    if (std::strcmp(sub, "add") == 0) {
        if (argc != 4) {
            return usage("wifi add <ssid> <password>   (quote names with spaces)");
        }
        return check(net::add_network(argv[2], argv[3]));
    }
    if (std::strcmp(sub, "connect") == 0) {
        if (argc != 4) {
            return usage("wifi connect <ssid> <password>   (saves it and connects now)");
        }
        return check(net::add_network(argv[2], argv[3], true));
    }
    if (std::strcmp(sub, "remove") == 0) {
        if (argc != 3 || !position(argv[2], index)) {
            return usage("wifi remove <position>");
        }
        return check(net::remove_network(index));
    }
    if (std::strcmp(sub, "up") == 0 || std::strcmp(sub, "down") == 0) {
        if (argc != 3 || !position(argv[2], index)) {
            return usage("wifi up|down <position>");
        }
        return check(net::move_network(index, sub[0] == 'u' ? -1 : 1));
    }
    if (std::strcmp(sub, "policy") == 0) {
        if (argc != 3 || (std::strcmp(argv[2], "order") != 0 && std::strcmp(argv[2], "strongest") != 0)) {
            return usage("wifi policy order|strongest");
        }
        return check(net::set_policy(argv[2][0] == 'o' ? net::Policy::EnteredOrder : net::Policy::StrongestSignal));
    }
    if (std::strcmp(sub, "scan") == 0) {
        std::printf("scanning, see 'wifi results' in a few seconds\n");
        return check(net::scan());
    }
    if (std::strcmp(sub, "results") == 0) {
        print_scan();
        return 0;
    }
    if (std::strcmp(sub, "reconnect") == 0) {
        return check(net::reconnect());
    }
    return usage("wifi [status|list|add|connect|remove|up|down|policy|scan|results|reconnect]");
}

int cmd_hotspot(int argc, char **argv)
{
    if (argc == 2 && std::strcmp(argv[1], "on") == 0) {
        return check(net::set_hotspot(true));
    }
    if (argc == 2 && std::strcmp(argv[1], "off") == 0) {
        return check(net::set_hotspot(false));
    }
    if (argc == 2 && std::strcmp(argv[1], "password") == 0) {
        return check(net::new_hotspot_password());
    }
    return usage("hotspot on|off|password");
}

int cmd_fs(int, char **)
{
    const auto &s = px::platform::storage_status();
    switch (s.fs) {
    case px::platform::FsState::Mounted:
        std::printf("LittleFS at %s: %zu of %zu KB used\n", px::platform::kFsMountPoint, s.fs_used / 1024,
                    s.fs_total / 1024);
        break;
    case px::platform::FsState::MountFailed:
        std::printf("LittleFS cannot be mounted (not formatted automatically)\n");
        break;
    case px::platform::FsState::NoPartition:
        std::printf("no storage partition\n");
        break;
    }
    return 0;
}

int cmd_login_reset(int argc, char **argv)
{
    if (argc != 2 || std::strcmp(argv[1], "yes") != 0) {
        return usage("login-reset yes   (removes the web UI login, the web UI is open until a new one exists)");
    }
    return check(px::webserver::reset_login());
}

int cmd_restart(int, char **)
{
    esp_restart();
    return 0;
}

// Heap per kind of memory: free now and the largest block one allocation can get (KB). Internal
// includes IRAM on the ESP32, which data cannot use; 8-bit is the part that can hold data. No
// "lowest since the start": ESP-IDF sums the low point of every region, so one region that was
// full once reads as 0 although the others had room.
int cmd_mem(int, char **)
{
    struct Row {
        const char *name;
        uint32_t caps;
    };
    static constexpr Row kRows[] = {{"internal", MALLOC_CAP_INTERNAL},
                                    {"internal 8-bit", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT},
                                    {"DMA", MALLOC_CAP_DMA},
                                    {"PSRAM", MALLOC_CAP_SPIRAM}};
    std::printf("%-15s %8s %8s\n", "", "free", "largest");
    for (const auto &row : kRows) {
        multi_heap_info_t info{};
        heap_caps_get_info(&info, row.caps);
        if (info.total_free_bytes + info.total_allocated_bytes == 0) {
            continue;
        }
        std::printf("%-15s %5u KB %5u KB\n", row.name, static_cast<unsigned>(info.total_free_bytes / 1024),
                    static_cast<unsigned>(info.largest_free_block / 1024));
    }
    return 0;
}

// ------------------------------------------------------------------------------ log output

constexpr const char *kSettingsNamespace = "console";
constexpr const char *kKeyLogLevel = "log_level";

struct LevelName {
    const char *name;
    esp_log_level_t level;
};

constexpr LevelName kLevels[] = {
    {"off", ESP_LOG_NONE}, {"errors", ESP_LOG_ERROR}, {"warnings", ESP_LOG_WARN},
    {"on", ESP_LOG_INFO},  {"info", ESP_LOG_INFO},    {"debug", ESP_LOG_DEBUG},
};

esp_log_level_t g_log_level = ESP_LOG_INFO;

const char *level_name(esp_log_level_t level)
{
    for (const auto &entry : kLevels) {
        if (entry.level == level) {
            return entry.name;
        }
    }
    return "?";
}

// The level for every tag, stored so the console stays quiet after a restart as well. The boot
// messages before the console starts are not affected.
void apply_log_level(esp_log_level_t level, bool store)
{
    g_log_level = level;
    // the web UI's log keeps its own level
    px::logbuf::set_console_level(level, level);
    if (store) {
        px::platform::Settings settings(kSettingsNamespace, px::platform::Settings::Mode::ReadWrite);
        if (settings.set_u8(kKeyLogLevel, static_cast<uint8_t>(level)) == ESP_OK) {
            settings.commit();
        }
    }
}

int cmd_log(int argc, char **argv)
{
    if (argc == 1) {
        // toggle between quiet and the normal output
        apply_log_level(g_log_level == ESP_LOG_NONE ? ESP_LOG_INFO : ESP_LOG_NONE, true);
        std::printf("log output %s\n", level_name(g_log_level));
        return 0;
    }
    if (argc == 2) {
        for (const auto &entry : kLevels) {
            if (std::strcmp(argv[1], entry.name) == 0) {
                apply_log_level(entry.level, true);
                std::printf("log output %s\n", level_name(g_log_level));
                return 0;
            }
        }
    }
    return usage("log [off|errors|warnings|on|debug]   (without an argument: toggle off/on)");
}

void restore_log_level()
{
    px::platform::Settings settings(kSettingsNamespace, px::platform::Settings::Mode::Read);
    uint8_t value = 0;
    if (settings.ok() && settings.get_u8(kKeyLogLevel, value) == ESP_OK && value <= ESP_LOG_VERBOSE) {
        apply_log_level(static_cast<esp_log_level_t>(value), false);
    } else {
        apply_log_level(g_log_level, false);
    }
}

int print_ldr()
{
    float mv = 0.0f;
    float ohms = 0.0f;
    float lux = 0.0f;
    float default_lux = 0.0f;
    if (!px::light_sensor::ldr::reading(mv, ohms) || !px::light_sensor::ldr::lux(lux) ||
        !px::light_sensor::ldr::default_lux(default_lux)) {
        std::printf("LDR: no reading yet\n");
        return 1;
    }
    px::light_sensor::ldr::Point points[px::light_sensor::ldr::kMaxPoints];
    const size_t count = px::light_sensor::ldr::points(points, px::light_sensor::ldr::kMaxPoints);
    std::printf("LDR %.0f mV, %.0f Ohm -> %.1f lx (%u points; default curve %.1f lx)\n", static_cast<double>(mv),
                static_cast<double>(ohms), static_cast<double>(lux), static_cast<unsigned>(count),
                static_cast<double>(default_lux));
    return 0;
}

// one line per second: the numbers to put next to a reference lux meter
int watch_ldr(int seconds)
{
    for (int i = 0; i < seconds; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (print_ldr() != 0) {
            return 1;
        }
    }
    return 0;
}

int list_points()
{
    px::light_sensor::ldr::Point points[px::light_sensor::ldr::kMaxPoints];
    const size_t count = px::light_sensor::ldr::points(points, px::light_sensor::ldr::kMaxPoints);
    for (size_t i = 0; i < count; i++) {
        std::printf("%8.0f Ohm = %8.1f lx\n", static_cast<double>(points[i].ohms), static_cast<double>(points[i].lux));
    }
    std::printf("%u points\n", static_cast<unsigned>(count));
    return 0;
}

int print_light()
{
    if (px::light_sensor::ldr::active()) {
        return print_ldr();
    }
    float raw = 0.0f;
    float lux = 0.0f;
    if (!px::light_sensor::raw_lux(raw) || !px::light_sensor::lux(lux)) {
        std::printf("no light sensor, or it does not answer\n");
        return 1;
    }
    bool calibrated = false;
    const float scale = px::light_sensor::scale(&calibrated);
    std::printf("measured %.1f lx, scale %.3f (%s) -> %.1f lx\n", static_cast<double>(raw), static_cast<double>(scale),
                calibrated ? "this device, from `light calibrate`" : "board default", static_cast<double>(lux));
    return 0;
}

int cmd_light(int argc, char **argv)
{
    if (argc == 1) {
        return print_light();
    }
    if (argc == 3 && std::strcmp(argv[1], "calibrate") == 0) {
        const float reference = std::strtof(argv[2], nullptr);
        if (check(px::light_sensor::calibrate(reference)) != 0) {
            return 1;
        }
        if (!px::light_sensor::ldr::active()) { // the LDR keeps a table of points, see `light points`
            std::printf("board info: .light_sensor_scale = %.3ff\n", static_cast<double>(px::light_sensor::scale()));
        }
        return print_light();
    }
    if (argc == 2 && std::strcmp(argv[1], "reset") == 0) {
        return check(px::light_sensor::reset_calibration());
    }
    if (px::light_sensor::ldr::active() && argc >= 2 && std::strcmp(argv[1], "watch") == 0) {
        return watch_ldr(argc == 3 ? std::atoi(argv[2]) : 10);
    }
    if (px::light_sensor::ldr::active() && argc == 4 && std::strcmp(argv[1], "point") == 0) {
        return check(px::light_sensor::ldr::add_point(std::strtof(argv[2], nullptr), std::strtof(argv[3], nullptr)));
    }
    if (px::light_sensor::ldr::active() && argc == 2 && std::strcmp(argv[1], "points") == 0) {
        return list_points();
    }
    return usage("light [calibrate <lux of a reference meter>|reset|watch [seconds]|points|point <ohms> <lux>]");
}

// The LED outputs before the render task exists (docs/leds.md section 2): the lanes of the board
// with a pixel count each, a frame held here, single pixels or all of them set and sent at once.
// For bring-up and for finding how a panel is wired.
// from malloc: new aborts when the heap is full (no exceptions), the frame of a large test may not
// fit
struct FreeDeleter {
    void operator()(uint8_t *pointer) const
    {
        std::free(pointer);
    }
};
std::unique_ptr<uint8_t[], FreeDeleter> g_frame;
std::unique_ptr<uint8_t[], FreeDeleter> g_limited;
// the frame send_frame() showed last, g_frame or g_limited; nullptr while it could not be scaled
const uint8_t *g_send = nullptr;
size_t g_frame_size = 0;
uint32_t g_frame_mw = 0;

// The power estimate of docs/leds.md section 3: a WS2812B channel at full draws about 16 mA at
// 5 V, an LED about 1 mA while dark (the kfc_fw defaults). Every frame is scaled down to the limit:
// the panels of the test matrix must stay below 10 W.
constexpr uint32_t kChannelMw = 80;
constexpr uint32_t kIdleMw = 5;
constexpr uint32_t kLimitMw = 10000;

int send_frame()
{
    const size_t pixels = g_frame_size / 3;
    uint64_t sum = 0;
    for (size_t i = 0; i < g_frame_size; i++) {
        sum += g_frame[i];
    }
    const uint32_t idle = static_cast<uint32_t>(pixels) * kIdleMw;
    const auto colors = static_cast<uint32_t>(sum * kChannelMw / 255);
    g_send = g_frame.get();
    g_frame_mw = idle + colors;
    // a black frame is sent as it is: above the limit it is the idle power of many LEDs alone,
    // which no scale lowers
    if (g_frame_mw > kLimitMw && colors > 0) {
        g_send = nullptr;
        // what is left above the idle power, as a scale in 1/65536
        const uint32_t room = kLimitMw > idle ? kLimitMw - idle : 0;
        const auto scale = static_cast<uint32_t>(static_cast<uint64_t>(room) * 65536 / colors);
        if (!g_limited) {
            g_limited.reset(static_cast<uint8_t *>(std::malloc(g_frame_size)));
            if (!g_limited) {
                std::printf("no memory for the scaled frame (%u bytes)\n", static_cast<unsigned>(g_frame_size));
                return 1;
            }
        }
        for (size_t i = 0; i < g_frame_size; i++) {
            g_limited[i] = static_cast<uint8_t>((g_frame[i] * scale) >> 16);
        }
        g_send = g_limited.get();
        std::printf("limited: %lu mW asked, scaled to %lu mW\n", static_cast<unsigned long>(g_frame_mw),
                    static_cast<unsigned long>(idle + static_cast<uint32_t>(static_cast<uint64_t>(colors) * scale >> 16)));
    }
    return check(px::output::show(g_send, g_frame_size));
}

// sends the frame `count` times back to back: what the output reaches without rendering
int bench_frames(int count)
{
    if (!g_frame) {
        std::printf("no lanes: leds setup or leds layout first\n");
        return 1;
    }
    // the frame as shown, scaled to the limit: the unscaled one sent full white to the test matrix
    // (2026-10-21, far above its 10 W, the board browned out)
    if (g_send == nullptr) {
        std::printf("the frame is above the power limit and was not scaled: leds fill first\n");
        return 1;
    }
    const uint8_t *frame = g_send;
    const int64_t start = esp_timer_get_time();
    for (int i = 0; i < count; i++) {
        if (px::output::show(frame, g_frame_size) != ESP_OK) {
            std::printf("frame %d failed\n", i);
            return 1;
        }
    }
    const int64_t us = esp_timer_get_time() - start;
    std::printf("%d frames of %u LEDs in %lld ms: %.1f fps, %lld us per frame\n", count,
                static_cast<unsigned>(g_frame_size / 3), us / 1000, static_cast<double>(count) * 1e6 / static_cast<double>(us),
                us / count);
    if (px::output::driver() == px::output::Driver::Parallel) {
        std::printf("encoder: longest interrupt of the last frame %lu us\n",
                    static_cast<unsigned long>(px::output::encoder_max_us()));
    }
    return 0;
}

// the parallel encoder alone on the slowest data (every bit set), nothing sent
int bench_encoder()
{
    const auto us = px::output::encoder_test();
    if (!us) {
        return check(us.error());
    }
    std::printf("encoder, every bit set on %u lanes: longest buffer %lu us (a buffer is sent in 720 us)\n",
                static_cast<unsigned>(px::output::lane_count()), static_cast<unsigned long>(*us));
    return 0;
}

// lanes 1..count of the board with these pixel counts, a black frame
int setup_leds(const int *counts, size_t count)
{
    namespace out = px::output;
    const auto &pins = px::board::info().led_gpios;
    out::Lane lanes[out::kMaxLanes] = {};
    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        if (i >= pins.size() || pins[i] < 0 || counts[i] < 1 || counts[i] > 2048) {
            std::printf("lane %u: the board has no such lane, or the count is not 1 to 2048\n", static_cast<unsigned>(i + 1));
            return 1;
        }
        lanes[i] = {.gpio = pins[i], .pixels = static_cast<uint16_t>(counts[i])};
        total += static_cast<size_t>(counts[i]);
    }
    if (check(out::start(lanes, count, px::leds::config().driver)) != 0) {
        return 1;
    }
    // the old frames first: 8 x 2048 LEDs (48 KB) nearly fill the largest free block of an ESP32
    // without PSRAM
    g_send = nullptr;
    g_frame.reset();
    g_limited.reset();
    g_frame_size = total * 3;
    g_frame.reset(static_cast<uint8_t *>(std::calloc(g_frame_size, 1)));
    if (!g_frame) {
        std::printf("no memory for a frame of %u bytes\n", static_cast<unsigned>(g_frame_size));
        g_frame_size = 0;
        return 1;
    }
    return send_frame();
}

// The render task on the console (px::leds): the layout from its panels, the state, the config.
// The output commands above (setup, fill, px, bench) suspend it; `leds resume` gives the lanes
// back.
px::leds::Layout g_console_layout;

bool parse_corner(const char *text, px::layout::Corner &out)
{
    using px::layout::Corner;
    static constexpr struct {
        const char *name;
        Corner corner;
    } kCorners[] = {{"tl", Corner::TopLeft}, {"tr", Corner::TopRight}, {"bl", Corner::BottomLeft}, {"br", Corner::BottomRight}};
    for (const auto &c : kCorners) {
        if (std::strcmp(text, c.name) == 0) {
            out = c.corner;
            return true;
        }
    }
    return false;
}

constexpr const char *kPowerOn[] = {"restore", "off", "on"};

void print_leds_status()
{
    const px::leds::Status s = px::leds::status();
    const px::effects::Info *effect = px::effects::info(s.state.effect);
    std::printf("%s, brightness %u (now %u), %s, color %u,%u,%u, speed %u, rotation %u%s (strength %u, attack %u ms, "
                "release %u ms)%s\n",
                s.state.on ? "on" : "off", s.state.brightness, s.level, effect != nullptr ? effect->slug : "?",
                s.state.color.r, s.state.color.g, s.state.color.b, s.state.speed, s.style.rotation * 90u,
                s.style.reactive ? ", reacts to sound" : "", s.style.strength, s.style.attack_ms, s.style.release_ms,
                s.has_audio ? (s.hears_audio ? " (hears sound)" : " (silence)") : "");
    std::printf("canvas %ux%u, %lu LEDs on %u lanes (%s), supply %s, %u fps, %lu mW%s%s%s\n", s.width, s.height,
                static_cast<unsigned long>(s.leds), s.lanes,
                px::leds::config().driver == px::output::Driver::Parallel ? "parallel" : "RMT", s.supply_on ? "on" : "off",
                s.fps,
                static_cast<unsigned long>(s.power_mw), s.limited ? " (limited)" : "", s.error[0] != 0 ? ", " : "",
                s.error);
    if (s.has_motion) {
        const px::leds::AutoOff a = px::leds::auto_off();
        std::printf("motion %s; auto off %s after %u min without motion%s%s\n", s.motion ? "detected" : "clear",
                    a.enabled ? "on" : "off", a.minutes, s.auto_off_in_s > 0 ? ", counting down" : "",
                    s.off_by_motion ? ", switched off by motion" : "");
        if (s.auto_off_in_s > 0) {
            std::printf("off in %lu s\n", static_cast<unsigned long>(s.auto_off_in_s));
        }
    }
    const px::leds::Config c = px::leds::config();
    std::printf("frame: animation %lu us, compose %lu us, output %lu us; target %u fps, dither %s, crossfade %u ms, "
                "power on: %s\n",
                static_cast<unsigned long>(s.render_us), static_cast<unsigned long>(s.compose_us),
                static_cast<unsigned long>(s.send_us), c.fps, c.dither ? "on" : "off", c.crossfade_ms,
                kPowerOn[static_cast<size_t>(c.power_on) % std::size(kPowerOn)]);
    const px::leds::Overlay o = px::leds::overlay();
    if (o.mode != px::leds::OverlayMode::Off) {
        std::printf("overlay: %s \"%s\", color %u,%u,%u, speed %u\n", o.mode == px::leds::OverlayMode::Text ? "text" : "clock",
                    o.text, o.color.r, o.color.g, o.color.b, o.speed);
    }
}

// the config with one change; the problem printed
int change_config(px::leds::Config config)
{
    char error[64] = {};
    if (!px::leds::set_config(config, error, sizeof(error))) {
        std::printf("%s\n", error);
        return 1;
    }
    return 0;
}

int cmd_render(int argc, char **argv)
{
    namespace leds = px::leds;
    const char *sub = argv[1];
    if (std::strcmp(sub, "canvas") == 0 && argc == 4) {
        g_console_layout = {};
        g_console_layout.width = static_cast<uint16_t>(std::atoi(argv[2]));
        g_console_layout.height = static_cast<uint16_t>(std::atoi(argv[3]));
        return 0;
    }
    if (std::strcmp(sub, "panel") == 0 && argc >= 9) {
        px::layout::Panel p{};
        p.lane = static_cast<uint8_t>(std::atoi(argv[2]) - 1);
        p.x = static_cast<uint16_t>(std::atoi(argv[3]));
        p.y = static_cast<uint16_t>(std::atoi(argv[4]));
        p.width = static_cast<uint16_t>(std::atoi(argv[5]));
        p.height = static_cast<uint16_t>(std::atoi(argv[6]));
        p.columns = std::strcmp(argv[8], "columns") == 0;
        p.zigzag = argc < 10 || std::strcmp(argv[9], "straight") != 0;
        if (std::atoi(argv[2]) < 1 || !parse_corner(argv[7], p.first) ||
            (!p.columns && std::strcmp(argv[8], "rows") != 0)) {
            return usage("leds panel <lane> <x> <y> <width> <height> tl|tr|bl|br rows|columns [zigzag|straight]");
        }
        if (g_console_layout.panel_count >= leds::kMaxPanels) {
            return usage("at most 16 panels");
        }
        g_console_layout.panels[g_console_layout.panel_count++] = p;
        return 0;
    }
    if (std::strcmp(sub, "layout") == 0) {
        char error[96] = {};
        if (!leds::set_layout(g_console_layout, error, sizeof(error))) {
            std::printf("%s\n", error);
            return 1;
        }
        return 0;
    }
    if (std::strcmp(sub, "on") == 0 || std::strcmp(sub, "off") == 0) {
        leds::set_on(std::strcmp(sub, "on") == 0);
        return 0;
    }
    if (std::strcmp(sub, "bright") == 0 && argc == 3) {
        leds::set_brightness(static_cast<uint8_t>(std::atoi(argv[2])));
        return 0;
    }
    if (std::strcmp(sub, "speed") == 0 && argc == 3) {
        leds::set_speed(static_cast<uint8_t>(std::atoi(argv[2])));
        return 0;
    }
    if (std::strcmp(sub, "color") == 0 && argc == 5) {
        leds::set_color({static_cast<uint8_t>(std::atoi(argv[2])), static_cast<uint8_t>(std::atoi(argv[3])),
                         static_cast<uint8_t>(std::atoi(argv[4]))});
        return 0;
    }
    if (std::strcmp(sub, "effect") == 0) {
        const size_t index = argc == 3 ? px::effects::find(argv[2]) : px::effects::count();
        if (index >= px::effects::count()) {
            std::printf("effects:");
            for (size_t i = 0; i < px::effects::count(); i++) {
                std::printf(" %s", px::effects::info(i)->slug);
            }
            std::printf("\n");
            return argc == 3 ? 1 : 0;
        }
        leds::set_effect(index);
        return 0;
    }
    if (std::strcmp(sub, "rotate") == 0 || std::strcmp(sub, "reactive") == 0 || std::strcmp(sub, "strength") == 0 ||
        std::strcmp(sub, "attack") == 0 || std::strcmp(sub, "release") == 0) {
        const size_t effect = leds::status().state.effect;
        leds::Style style = leds::styles().style[effect < leds::kMaxEffects ? effect : 0];
        if (sub[0] == 'a' || sub[2] == 'l') {
            const bool attack = sub[0] == 'a';
            const int ms = argc == 3 ? std::atoi(argv[2]) : -1;
            if (ms < 0 || ms > (attack ? px::effects::kMaxAttackMs : px::effects::kMaxReleaseMs)) {
                return usage(attack ? "leds attack <0-2000 ms>   (the current animation; 20 as built)"
                                    : "leds release <0-5000 ms>   (the current animation; 250 as built)");
            }
            (attack ? style.attack_ms : style.release_ms) = static_cast<uint16_t>(ms);
        } else if (sub[0] == 's') {
            const int strength = argc == 3 ? std::atoi(argv[2]) : -1;
            if (strength < 0 || strength > 255) {
                return usage("leds strength <0-255>   (the current animation; 128 as built)");
            }
            style.strength = static_cast<uint8_t>(strength);
        } else if (sub[1] == 'o') {
            const int degrees = argc == 3 ? std::atoi(argv[2]) : -1;
            if (degrees < 0 || degrees > 270 || degrees % 90 != 0) {
                return usage("leds rotate 0|90|180|270   (the current animation)");
            }
            style.rotation = static_cast<uint8_t>(degrees / 90);
        } else {
            if (argc != 3 || (std::strcmp(argv[2], "on") != 0 && std::strcmp(argv[2], "off") != 0)) {
                return usage("leds reactive on|off   (the current animation)");
            }
            style.reactive = std::strcmp(argv[2], "on") == 0;
        }
        leds::set_style(effect, style);
        return 0;
    }
    if (std::strcmp(sub, "driver") == 0 && argc == 3) {
        if (std::strcmp(argv[2], "parallel") != 0 && std::strcmp(argv[2], "rmt") != 0) {
            return usage("leds driver rmt|parallel");
        }
        leds::Config config = leds::config();
        config.driver = std::strcmp(argv[2], "parallel") == 0 ? px::output::Driver::Parallel : px::output::Driver::Rmt;
        char error[64] = {};
        if (!leds::set_config(config, error, sizeof(error))) {
            std::printf("%s\n", error);
            return 1;
        }
        return 0;
    }
    if (std::strcmp(sub, "lane") == 0) {
        static constexpr const char *kOrders[] = {"grb", "rgb", "brg", "rbg", "gbr", "bgr"};
        leds::Lanes lanes = leds::lanes();
        if (argc == 2) {
            for (size_t i = 0; i < leds::status().available_lanes; i++) {
                const leds::LaneSettings &l = lanes.lane[i];
                std::printf("lane %u: %s, level %u, balance %u %u %u, dither %s\n", static_cast<unsigned>(i + 1),
                            kOrders[static_cast<size_t>(l.order)], l.level, l.red, l.green, l.blue,
                            l.dither ? "on" : "off");
            }
            return 0;
        }
        const int n = argc >= 4 ? std::atoi(argv[2]) : 0;
        if (n < 1 || n > static_cast<int>(px::output::kMaxLanes)) {
            return usage("leds lane [<n> order grb|rgb|brg|rbg|gbr|bgr | level <0-255> | balance <r> <g> <b> | dither on|off]");
        }
        leds::LaneSettings &l = lanes.lane[n - 1];
        const char *what = argv[3];
        if (std::strcmp(what, "dither") == 0 && argc == 5) {
            l.dither = std::strcmp(argv[4], "on") == 0;
        } else if (std::strcmp(what, "level") == 0 && argc == 5) {
            l.level = static_cast<uint8_t>(std::atoi(argv[4]));
        } else if (std::strcmp(what, "balance") == 0 && argc == 7) {
            l.red = static_cast<uint8_t>(std::atoi(argv[4]));
            l.green = static_cast<uint8_t>(std::atoi(argv[5]));
            l.blue = static_cast<uint8_t>(std::atoi(argv[6]));
        } else if (std::strcmp(what, "order") == 0 && argc == 5) {
            size_t order = 0;
            while (order < std::size(kOrders) && std::strcmp(argv[4], kOrders[order]) != 0) {
                order++;
            }
            if (order == std::size(kOrders)) {
                return usage("order: grb, rgb, brg, rbg, gbr or bgr");
            }
            l.order = static_cast<px::output::ColorOrder>(order);
        } else {
            return usage("leds lane <n> order <grb> | level <0-255> | balance <r> <g> <b> | dither on|off");
        }
        leds::set_lanes(lanes);
        return 0;
    }
    if (std::strcmp(sub, "autooff") == 0 && argc == 2) {
        const leds::AutoOff a = leds::auto_off();
        std::printf("auto off %s, %u min without motion%s\n", a.enabled ? "on" : "off", a.minutes,
                    leds::status().has_motion ? "" : " (this board has no motion sensor)");
        return 0;
    }
    if (std::strcmp(sub, "autooff") == 0 && argc == 3) {
        leds::AutoOff a = leds::auto_off();
        if (std::strcmp(argv[2], "on") == 0 || std::strcmp(argv[2], "off") == 0) {
            a.enabled = argv[2][1] == 'n';
        } else if (std::atoi(argv[2]) >= 1 && std::atoi(argv[2]) <= leds::kMaxAutoOffMinutes) {
            a.minutes = static_cast<uint16_t>(std::atoi(argv[2]));
        } else {
            return usage("leds autooff on|off|<minutes 1-240>");
        }
        leds::set_auto_off(a);
        return 0;
    }
    if (std::strcmp(sub, "fps") == 0 && argc == 3) {
        leds::Config config = leds::config();
        config.fps = static_cast<uint8_t>(std::atoi(argv[2]));
        return change_config(config);
    }
    if (std::strcmp(sub, "crossfade") == 0 && argc == 3) {
        leds::Config config = leds::config();
        config.crossfade_ms = static_cast<uint16_t>(std::atoi(argv[2]));
        return change_config(config);
    }
    if (std::strcmp(sub, "poweron") == 0) {
        size_t mode = 0;
        while (argc == 3 && mode < std::size(kPowerOn) && std::strcmp(argv[2], kPowerOn[mode]) != 0) {
            mode++;
        }
        if (argc != 3 || mode == std::size(kPowerOn)) {
            return usage("leds poweron restore|off|on   (at the start: as before, off or on)");
        }
        leds::Config config = leds::config();
        config.power_on = static_cast<leds::PowerOn>(mode);
        return change_config(config);
    }
    if (std::strcmp(sub, "overlay") == 0) {
        leds::Overlay o = leds::overlay();
        if (argc >= 3 && std::strcmp(argv[2], "off") == 0) {
            o.mode = leds::OverlayMode::Off;
        } else if (argc >= 3 && std::strcmp(argv[2], "clock") == 0) {
            o.mode = leds::OverlayMode::Clock;
            o.clock_24h = argc < 4 || std::strcmp(argv[3], "12") != 0;
        } else if (argc >= 4 && std::strcmp(argv[2], "text") == 0) {
            // the words after "text" with single spaces
            o.mode = leds::OverlayMode::Text;
            o.text[0] = 0;
            for (int i = 3; i < argc; i++) {
                const size_t used = std::strlen(o.text);
                std::snprintf(o.text + used, sizeof(o.text) - used, "%s%s", i > 3 ? " " : "", argv[i]);
            }
        } else if (argc == 6 && std::strcmp(argv[2], "color") == 0) {
            o.color = {static_cast<uint8_t>(std::atoi(argv[3])), static_cast<uint8_t>(std::atoi(argv[4])),
                       static_cast<uint8_t>(std::atoi(argv[5]))};
        } else if (argc == 4 && std::strcmp(argv[2], "speed") == 0) {
            o.speed = static_cast<uint8_t>(std::atoi(argv[3]));
        } else {
            return usage("leds overlay off | clock [24|12] | text <words> | color <r> <g> <b> | speed <0-255>");
        }
        leds::set_overlay(o);
        return 0;
    }
    if (std::strcmp(sub, "dither") == 0 && argc == 3) {
        leds::Config config = leds::config();
        config.dither = std::strcmp(argv[2], "on") == 0;
        char error[64] = {};
        if (!leds::set_config(config, error, sizeof(error))) {
            std::printf("%s\n", error);
            return 1;
        }
        return 0;
    }
    if (std::strcmp(sub, "limit") == 0 && argc == 3) {
        leds::Config config = leds::config();
        config.power_limit_mw = static_cast<uint32_t>(std::atoi(argv[2]));
        char error[64] = {};
        if (!leds::set_config(config, error, sizeof(error))) {
            std::printf("%s\n", error);
            return 1;
        }
        return 0;
    }
    if (std::strcmp(sub, "resume") == 0) {
        g_send = nullptr;
        g_frame.reset();
        g_limited.reset();
        g_frame_size = 0;
        leds::suspend(false);
        return 0;
    }
    if (std::strcmp(sub, "status") == 0) {
        print_leds_status();
        return 0;
    }
    return -1;
}

int cmd_leds(int argc, char **argv)
{
    namespace out = px::output;
    const char *sub = argc > 1 ? argv[1] : "status";
    if (argc > 1) {
        if (const int result = cmd_render(argc, argv); result >= 0) {
            return result;
        }
    }
    if (argc == 1) {
        print_leds_status();
        return 0;
    }
    // Only the output commands below drive the lanes themselves and suspend the render task; any
    // other word is a typo and must not stop the LEDs (`leds autooff` without its value did).
    static constexpr const char *kOutputCommands[] = {"setup", "fill", "px", "bench", "stop"};
    const bool output_command =
        std::atoi(sub) > 0 ||
        std::any_of(std::begin(kOutputCommands), std::end(kOutputCommands),
                    [&](const char *name) { return std::strcmp(sub, name) == 0; });
    if (output_command) {
        px::leds::suspend(true);
    } else if (std::strcmp(sub, "lanes") != 0) {
        return usage("leds [status] | on | off | bright <0-255> | effect [<name>] | color <r> <g> <b> | speed <0-255> |"
                     " rotate 0|90|180|270 | reactive on|off | strength <0-255> | attack <ms> | release <ms> | autooff on|off|<min> |"
                     " fps <n> | crossfade <ms> | poweron restore|off|on | overlay ... | dither on|off | driver rmt|parallel | limit <mW> |"
                     " lane [<n> ...] | canvas | panel | layout -- output tests (suspend the LEDs until `leds resume`):"
                     " setup | fill | px | bench | lanes | stop");
    }
    if (std::strcmp(sub, "bench") == 0 && argc > 2 && std::strcmp(argv[2], "encode") == 0) {
        return bench_encoder();
    }
    if (std::strcmp(sub, "bench") == 0) {
        return bench_frames(argc > 2 ? std::atoi(argv[2]) : 200);
    }
    if (std::strcmp(sub, "stop") == 0) {
        out::stop();
        g_send = nullptr;
        g_frame.reset();
        g_limited.reset();
        g_frame_size = 0;
        return 0;
    }
    if (std::strcmp(sub, "lanes") == 0) {
        for (size_t i = 0; i < out::lane_count(); i++) {
            std::printf("lane %u on GPIO %d, %u pixels\n", static_cast<unsigned>(i + 1), out::lane(i).gpio,
                        out::lane(i).pixels);
        }
        if (out::running()) {
            std::printf("the frame asks for %lu mW, limit %lu mW\n", static_cast<unsigned long>(g_frame_mw),
                        static_cast<unsigned long>(kLimitMw));
        } else {
            std::printf("not started\n");
        }
        return 0;
    }
    if (std::strcmp(sub, "setup") == 0 && argc >= 3 && argc - 2 <= static_cast<int>(out::kMaxLanes)) {
        int counts[out::kMaxLanes];
        for (int i = 2; i < argc; i++) {
            counts[i - 2] = std::atoi(argv[i]);
        }
        return setup_leds(counts, static_cast<size_t>(argc - 2));
    }
    if (std::strcmp(sub, "fill") == 0 && argc == 5 && g_frame) {
        for (size_t i = 0; i < g_frame_size; i += 3) {
            g_frame[i] = static_cast<uint8_t>(std::atoi(argv[2]));
            g_frame[i + 1] = static_cast<uint8_t>(std::atoi(argv[3]));
            g_frame[i + 2] = static_cast<uint8_t>(std::atoi(argv[4]));
        }
        return send_frame();
    }
    if (std::strcmp(sub, "px") == 0 && argc == 7 && g_frame) {
        const int lane = std::atoi(argv[2]);
        if (lane < 1 || lane > static_cast<int>(out::lane_count())) {
            return usage("leds px <lane> <from>[-<to>] <r> <g> <b>");
        }
        size_t offset = 0;
        for (int i = 0; i < lane - 1; i++) {
            offset += out::lane(static_cast<size_t>(i)).pixels;
        }
        const int pixels = out::lane(static_cast<size_t>(lane - 1)).pixels;
        char *dash = nullptr;
        const long from = std::strtol(argv[3], &dash, 10);
        const long to = dash != nullptr && *dash == '-' ? std::strtol(dash + 1, nullptr, 10) : from;
        if (from < 0 || to < from || to >= pixels) {
            return usage("leds px <lane> <from>[-<to>] <r> <g> <b>   (pixels count from 0)");
        }
        for (long p = from; p <= to; p++) {
            uint8_t *rgb = &g_frame[(offset + static_cast<size_t>(p)) * 3];
            rgb[0] = static_cast<uint8_t>(std::atoi(argv[4]));
            rgb[1] = static_cast<uint8_t>(std::atoi(argv[5]));
            rgb[2] = static_cast<uint8_t>(std::atoi(argv[6]));
        }
        return send_frame();
    }
    // the short form: one color on the first <count> pixels of one lane
    if (argc >= 5 && argc <= 6 && std::atoi(argv[1]) > 0) {
        const int lane = argc == 6 ? std::atoi(argv[5]) : 1;
        if (lane < 1 || lane > static_cast<int>(out::kMaxLanes)) {
            return usage("lane 1 to 16");
        }
        int counts[out::kMaxLanes] = {};
        for (int i = 0; i < lane; i++) {
            counts[i] = i == lane - 1 ? std::atoi(argv[1]) : 1;
        }
        if (setup_leds(counts, static_cast<size_t>(lane)) != 0) {
            return 1;
        }
        const char *fill[7] = {"leds", "px", nullptr, nullptr, argv[2], argv[3], argv[4]};
        char lane_text[4];
        std::snprintf(lane_text, sizeof(lane_text), "%d", lane);
        char range[16];
        std::snprintf(range, sizeof(range), "0-%d", std::atoi(argv[1]) - 1);
        fill[2] = lane_text;
        fill[3] = range;
        return cmd_leds(7, const_cast<char **>(fill));
    }
    return usage("leds [status] | on | off | bright <0-255> | effect [<name>] | color <r> <g> <b> | speed <0-255> |"
                 " limit <mW> | canvas <w> <h> | panel <lane> <x> <y> <w> <h> tl|tr|bl|br rows|columns [zigzag|straight] |"
                 " layout   -- output tests (suspend the LEDs until `leds resume`): setup <count lane 1> ... | fill <r> <g> <b> |"
                 " px <lane> <from>[-<to>] <r> <g> <b> | <count> <r> <g> <b> [lane] | bench [frames|encode] | lanes | stop");
}

// The relays: their state, or one switched by hand
int cmd_relay(int argc, char **argv)
{
    if (!px::relay::running()) {
        std::printf("no relays\n");
        return 1;
    }
    if (argc == 1 || std::strcmp(argv[1], "status") == 0) {
        for (unsigned relay = 1; relay <= px::relay::kMaxRelays; relay++) {
            if (px::relay::gpio_of(relay) >= 0) {
                std::printf("relay %u on GPIO %d: %s\n", relay, px::relay::gpio_of(relay), px::relay::is_on(relay) ? "on" : "off");
            }
        }
        return 0;
    }
    const int relay = std::atoi(argv[1]);
    if (argc != 3 || relay < 1 || (std::strcmp(argv[2], "on") != 0 && std::strcmp(argv[2], "off") != 0)) {
        return usage("relay [status|<n> on|off]");
    }
    if (!px::relay::set(static_cast<unsigned>(relay), std::strcmp(argv[2], "on") == 0)) {
        std::printf("no relay %d\n", relay);
        return 1;
    }
    return 0;
}

// The motion sensor: its state
int cmd_motion(int argc, char **argv)
{
    if (!px::motion::running()) {
        std::printf("no motion sensor\n");
        return 1;
    }
    if (argc == 1 || std::strcmp(argv[1], "status") == 0) {
        std::printf("sensor on GPIO %d, active %s: %s; Home Assistant: %s (hold %lu s)\n", px::motion::gpio(),
                    px::motion::active_high() ? "high" : "low", px::motion::occupied() ? "motion" : "none",
                    px::motion::detected() ? "detected" : "clear", static_cast<unsigned long>(px::motion::hold_s()));
        return 0;
    }
    return usage("motion [status]");
}

// The microphone: one line of the analysis, the bands as a bar of digits 0..9
void print_audio()
{
    const px::audio::Status s = px::audio::status();
    char bars[px::effects::kBands + 1];
    for (size_t b = 0; b < px::effects::kBands; b++) {
        bars[b] = static_cast<char>('0' + s.audio.bands[b] * 9 / 255);
    }
    bars[px::effects::kBands] = '\0';
    if (s.source == px::audio::Source::Warls || s.source == px::audio::Source::WledSync) {
        std::printf("%s %-5s %lu packets, loudness %3u%s |%s|\n", s.audio.active ? "sound  " : "silence",
                    px::audio::source_name(s.source), static_cast<unsigned long>(s.packets), s.audio.loudness,
                    s.audio.beat ? " BEAT" : "     ", bars);
        return;
    }
    std::printf("%s slot %c, loudest %6.1f dB, gain %6.1f dB, loudness %3u%s |%s|\n",
                s.audio.active ? "sound  " : "silence", s.slot, static_cast<double>(s.loudest_db),
                static_cast<double>(s.reference_db), s.audio.loudness, s.audio.beat ? " BEAT" : "     ", bars);
}

int cmd_audio(int argc, char **argv)
{
    if (!px::audio::running()) {
        std::printf("no audio input\n");
        return 1;
    }
    if (argc == 1 || std::strcmp(argv[1], "status") == 0) {
        const px::audio::Settings c = px::audio::settings();
        const px::audio::Inputs in = px::audio::inputs();
        std::printf("microphone %s; gain %d dB, automatic gain %s, silence below %d dB, range %u dB, beat at %u.%u x the "
                    "bass\n",
                    px::audio::has_microphone() ? "on" : "none", c.gain_db, c.agc ? "on" : "off", c.silence_db, c.range_db,
                    c.beat_ratio_tenths / 10u, c.beat_ratio_tenths % 10u);
        std::printf("WARLS %s (port %u), WLED audio sync %s; now from: %s\n", in.warls ? "on" : "off", in.warls_port,
                    in.wled_sync ? "on" : "off", px::audio::status().source == px::audio::Source::None
                                                     ? "nothing"
                                                     : px::audio::source_name(px::audio::status().source));
        print_audio();
        return 0;
    }
    if (std::strcmp(argv[1], "set") == 0 && argc == 4) {
        px::audio::Settings c = px::audio::settings();
        const char *key = argv[2];
        const int value = std::atoi(argv[3]);
        px::audio::Inputs in = px::audio::inputs();
        if (std::strcmp(key, "warls") == 0 || std::strcmp(key, "wled") == 0) {
            (key[1] == 'a' ? in.warls : in.wled_sync) = std::strcmp(argv[3], "on") == 0;
            px::audio::set_inputs(in);
            return 0;
        }
        if (std::strcmp(key, "port") == 0) {
            if (value < 1 || value > 65535) {
                return usage("audio set port <1-65535>   (WARLS; 21324 as built)");
            }
            in.warls_port = static_cast<uint16_t>(value);
            px::audio::set_inputs(in);
            return 0;
        }
        if (std::strcmp(key, "gain") == 0) {
            c.gain_db = static_cast<int8_t>(value);
        } else if (std::strcmp(key, "agc") == 0) {
            c.agc = std::strcmp(argv[3], "on") == 0;
        } else if (std::strcmp(key, "silence") == 0) {
            c.silence_db = static_cast<int8_t>(value);
        } else if (std::strcmp(key, "range") == 0) {
            c.range_db = static_cast<uint8_t>(value);
        } else if (std::strcmp(key, "beat") == 0) {
            // 1.6 or 16: tenths either way
            const double ratio = std::atof(argv[3]);
            c.beat_ratio_tenths = static_cast<uint8_t>(ratio < 10.0 ? ratio * 10.0 + 0.5 : ratio);
        } else {
            return usage("audio set gain <dB>|agc on|off|silence <dB>|range <dB>|beat <ratio>|warls on|off|port <n>|"
                         "wled on|off");
        }
        char error[64] = {};
        if (!px::audio::set_settings(c, error, sizeof(error))) {
            std::printf("%s\n", error);
            return 1;
        }
        return 0;
    }
    if (std::strcmp(argv[1], "watch") == 0) {
        // 5 lines a second
        const int seconds = argc == 3 ? std::atoi(argv[2]) : 10;
        for (int i = 0; i < seconds * 5; i++) {
            vTaskDelay(pdMS_TO_TICKS(200));
            print_audio();
        }
        return 0;
    }
    return usage("audio [status|watch [seconds]|set gain <dB>|agc on|off|silence <dB>|range <dB>|beat <ratio>|"
                 "warls on|off|port <n>|wled on|off]");
}

// A deliberate crash: tries the panic handler (the LED supply off) and the count of crashes in a
// row (docs/leds.md section 5)
int cmd_crash(int argc, char **argv)
{
    if (argc != 2 || std::strcmp(argv[1], "yes") != 0) {
        return usage("crash yes   (aborts: the panic handler runs, the device restarts)");
    }
    std::printf("crashing\n");
    std::fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(50));
    std::abort();
}

int cmd_factory_reset(int argc, char **argv)
{
    if (argc != 2 || std::strcmp(argv[1], "yes") != 0) {
        return usage("factory-reset yes   (erases every setting, then restarts)");
    }
    if (check(px::platform::erase_all_settings()) != 0) {
        return 1;
    }
    esp_restart();
    return 0;
}

esp_err_t add(const char *name, const char *help, const char *hint, esp_console_cmd_func_t func)
{
    esp_console_cmd_t command{};
    command.command = name;
    command.help = help;
    command.hint = hint;
    command.func = func;
    return esp_console_cmd_register(&command);
}

TaskHandle_t g_task = nullptr;

constexpr const char *kPrompt = "pixeled> ";
constexpr uint32_t kStackSize = 6144;

// The console's port with its driver, as esp_console's REPL sets it up: Enter sends CR, a line
// ends in CRLF. A UART, or the USB-Serial/JTAG of boards without a USB-UART bridge.
void init_port()
{
    fflush(stdout);
    fsync(fileno(stdout));
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);
    usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&config));
    usb_serial_jtag_vfs_use_driver();
#else
    uart_vfs_dev_port_set_rx_line_endings(CONFIG_ESP_CONSOLE_UART_NUM, ESP_LINE_ENDINGS_CR);
    uart_vfs_dev_port_set_tx_line_endings(CONFIG_ESP_CONSOLE_UART_NUM, ESP_LINE_ENDINGS_CRLF);
    uart_config_t config{};
    config.baud_rate = CONFIG_ESP_CONSOLE_UART_BAUDRATE;
    config.data_bits = UART_DATA_8_BITS;
    config.parity = UART_PARITY_DISABLE;
    config.stop_bits = UART_STOP_BITS_1;
    config.source_clk = UART_SCLK_DEFAULT;
    constexpr auto kUart = static_cast<uart_port_t>(CONFIG_ESP_CONSOLE_UART_NUM);
    ESP_ERROR_CHECK(uart_driver_install(kUart, 256, 0, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_param_config(kUart, &config));
    uart_vfs_dev_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);
#endif
    setvbuf(stdin, nullptr, _IONBF, 0);
#if CONFIG_LIBC_PICOLIBC
    tls_stdin = stdin;
    tls_stdout = stdout;
#endif
}

void init_console()
{
    esp_console_config_t config = ESP_CONSOLE_CONFIG_DEFAULT();
    config.max_cmdline_length = 256;
    config.max_cmdline_args = 12;
    ESP_ERROR_CHECK(esp_console_init(&config));
    linenoiseSetMultiLine(1);
    linenoiseSetCompletionCallback(&esp_console_get_completion);
    linenoiseSetHintsCallback(reinterpret_cast<linenoiseHintsCallback *>(&esp_console_get_hint));
    linenoiseHistorySetMaxLen(32);
    linenoiseSetMaxLineLen(config.max_cmdline_length);
    linenoiseAllowEmpty(false);
    // a terminal without escape sequences gets plain lines
    if (linenoiseProbe() != 0) {
        linenoiseSetDumbMode(1);
    }
}

void register_commands()
{
    esp_console_register_help_command();
    add("wifi", "Wi-Fi: status, saved networks, scan, reconnect", "[status|list|add|connect|remove|up|down|policy|scan|results|reconnect]",
        cmd_wifi);
    add("hotspot", "turn the hotspot on or off, or give it a new password", "on|off|password", cmd_hotspot);
    add("fs", "file system status", nullptr, cmd_fs);
    add("mem", "free memory: internal, DMA, PSRAM", nullptr, cmd_mem);
    add("restart", "restart the device", nullptr, cmd_restart);
    add("reboot", "restart the device", nullptr, cmd_restart);
    add("login-reset", "remove the web UI login", "yes", cmd_login_reset);
    add("log", "log output on the serial port: toggle, or set a level", "[off|errors|warnings|on|debug]", cmd_log);
    add("factory-reset", "erase every setting and restart", "yes", cmd_factory_reset);
    add("crash", "crash on purpose: tries the panic handler (LED supply off) and the crash count", "yes", cmd_crash);
    add("light", "light sensor: values, calibration while adding a board", "[calibrate <lux>|reset|watch [s]|points]",
        cmd_light);
    add("leds", "the LEDs: state, layout, and output tests", "[status] | on | off | bright <n> | effect [<name>] | color <r> <g> <b> | speed <n> | rotate <deg> | reactive on|off | strength <n> | attack <ms> | release <ms> | crossfade <ms> | poweron restore|off|on | overlay off|clock|text ... | limit <mW> | driver rmt|parallel | dither on|off | fps <n> | autooff on|off|<min> | lane [<n> ...] | canvas <w> <h> | panel ... | layout | setup <counts...> | fill | px | bench [n|encode] | lanes | resume", cmd_leds);
    add("relay", "the LED power switch: the state, or switched", "[status|<n> on|off]", cmd_relay);
    add("motion", "motion sensor: the state", "[status]", cmd_motion);
    add("audio", "microphone: the analysis and its settings",
        "[status|watch [s]|set gain|agc|silence|range|beat|warls|port|wled <value>]", cmd_audio);
}

// The console's own task: created at the start, so its stack is taken while the internal RAM is
// in one piece. The probe of the terminal runs here, not in app_main.
void console_task(void *)
{
    init_port();
    init_console();
    restore_log_level();
    register_commands();
    // the commands use the network and the other components: wait until they run
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    std::printf("\r\nType 'help' to get the list of commands.\r\n");
    for (;;) {
        char *line = linenoise(kPrompt);
        if (line == nullptr) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        linenoiseHistoryAdd(line);
        int result = 0;
        const esp_err_t err = esp_console_run(line, &result);
        if (err == ESP_ERR_NOT_FOUND) {
            std::printf("Unrecognized command\n");
        } else if (err == ESP_OK && result != 0) {
            std::printf("Command returned non-zero error code: 0x%x (%s)\n", result, esp_err_to_name(result));
        } else if (err != ESP_OK && err != ESP_ERR_INVALID_ARG) {
            std::printf("Internal error: %s\n", esp_err_to_name(err));
        }
        linenoiseFree(line);
    }
}

} // namespace

esp_err_t start()
{
    return xTaskCreate(console_task, "console", kStackSize, nullptr, 2, &g_task) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void ready()
{
    if (g_task != nullptr) {
        xTaskNotifyGive(g_task);
    }
}

#else

esp_err_t start()
{
    return ESP_ERR_NOT_SUPPORTED;
}

void ready()
{
}

#endif

} // namespace px::devconsole
