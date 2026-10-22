#include "backup.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

#include "esp_app_desc.h"
#include "px/audio/audio.hpp"
#include "px/board/board.hpp"
#include "px/config/backup.hpp"
#include "px/effects/effects.hpp"
#include "px/layout/layout.hpp"
#include "px/leds/leds.hpp"
#include "px/hass/hass.hpp"
#include "px/input/input.hpp"
#include "px/network/network.hpp"
#include "px/platform/time_sync.hpp"
#include "px/presets/presets.hpp"
#include "px/webserver/webserver.hpp"

namespace {

namespace backup = px::config::backup;
namespace net = px::network;
using px::formats::yaml::Kind;
using px::formats::yaml::Node;
using px::memory::String;

String text_of(const char *format, const char *a, const char *b = "")
{
    char text[160];
    std::snprintf(text, sizeof(text), format, a, b);
    return text;
}

// ---------------------------------------------------------------------------------- network

constexpr std::string_view kPolicies[] = {"order", "strongest"};

void write_network(backup::Writer &out)
{
    net::SavedNetworkInfo saved[net::kMaxNetworks];
    const size_t count = net::saved_networks(saved, net::kMaxNetworks);
    const net::Status status = net::status();
    out.section("network");
    out.text("policy", kPolicies[status.policy == net::Policy::StrongestSignal ? 1 : 0]);
    out.boolean("hotspot_on", status.hotspot_enabled);
    out.comment("passwords are not exported: add \"password: ...\" under a name by hand, or enter them on the device");
    out.open("networks");
    for (size_t i = 0; i < count; i++) {
        out.item("name", saved[i].ssid);
        if (saved[i].open) {
            out.boolean("open", true, 3);
        }
    }
}

// one network of the file; false when it has a problem (already in the report)
bool read_one_network(const Node &item, size_t number, net::ImportedNetwork &out, backup::Report &report)
{
    char path[32];
    std::snprintf(path, sizeof(path), "network.networks[%u]", static_cast<unsigned>(number));
    const size_t problems = report.problems.size();
    backup::Fields fields(item, path, report);
    String name, password;
    bool open = false;
    fields.text("name", net::kSsidSize - 1, name);
    const bool has_password = fields.text("password", net::kPasswordSize - 1, password);
    fields.boolean("open", open);
    fields.finish();
    if (report.problems.size() != problems) {
        return false;
    }
    if (name.empty()) {
        fields.problem("name", "the name of the network is missing");
        return false;
    }
    if (has_password && !password.empty() && password.size() < net::kMinPassword) {
        fields.problem("password", "at least 8 characters (none for an open network)");
        return false;
    }
    if (open && has_password && !password.empty()) {
        fields.problem("open", "an open network has no password");
        return false;
    }
    out = {};
    std::snprintf(out.ssid, sizeof(out.ssid), "%s", name.c_str());
    std::snprintf(out.password, sizeof(out.password), "%s", password.c_str());
    // open: saved without a password, nothing to enter later
    out.has_password = has_password || open;
    return true;
}

void read_network(const Node &value, bool apply, backup::Report &report)
{
    // the web server's task is the only caller; too large for its stack
    static net::ImportedNetwork imported[net::kMaxImport];
    static net::ImportResult result;

    const net::Status status = net::status();
    const size_t problems = report.problems.size();
    backup::Fields fields(value, "network", report);
    size_t policy = status.policy == net::Policy::StrongestSignal ? 1 : 0;
    const bool has_policy = fields.choice("policy", kPolicies, policy);
    bool hotspot = status.hotspot_enabled;
    const bool has_hotspot = fields.boolean("hotspot_on", hotspot);
    size_t count = 0;
    const Node *list = fields.node("networks");
    if (list != nullptr && list->kind != Kind::List) {
        fields.problem("networks", "a list of networks expected (- name: ...)");
    } else if (list != nullptr && list->list.size() > net::kMaxImport) {
        char message[48];
        std::snprintf(message, sizeof(message), "at most %u networks", static_cast<unsigned>(net::kMaxImport));
        fields.problem("networks", message);
    } else if (list != nullptr) {
        for (size_t i = 0; i < list->list.size(); i++) {
            if (!read_one_network(list->list[i], i + 1, imported[count], report)) {
                continue;
            }
            const bool twice = std::any_of(imported, imported + count, [&](const net::ImportedNetwork &other) {
                return std::strcmp(other.ssid, imported[count].ssid) == 0;
            });
            if (twice) {
                fields.problem("networks", text_of("'%s' is in the list twice", imported[count].ssid));
                continue;
            }
            count++;
        }
    }
    fields.finish();
    if (report.problems.size() != problems) {
        return;
    }

    if (list != nullptr) {
        // with `apply` false the network task only says what it would do
        if (net::import_networks(imported, count, apply, result) != ESP_OK) {
            report.problem("network", "the network task did not answer");
            return;
        }
        for (size_t i = 0; i < result.need_password_count; i++) {
            report.note("network", text_of("%s needs its password (Wi-Fi page)", result.need_password[i]));
        }
        for (size_t i = 0; i < result.dropped_count; i++) {
            report.note("network", text_of("%s is not saved, at most 5 networks", result.dropped[i]));
        }
    }
    if (apply && has_policy) {
        net::set_policy(policy == 1 ? net::Policy::StrongestSignal : net::Policy::EnteredOrder);
    }
    if (apply && has_hotspot) {
        net::set_hotspot(hotspot);
    }
}

// The web UI login (docs/ui/web/web-app.md section 3): import only. The export writes a hint in
// comments, never the section: the device stores a hash, the password is not there to write.
void write_webui(backup::Writer &out)
{
    out.comment("the web UI login is not exported; added by hand, an import sets it (and ends every session):", 0);
    out.comment("webui:", 0);
    out.comment("  user: admin", 0);
    out.comment("  password: 8-to-64-characters", 0);
}

void read_webui(const Node &value, bool apply, backup::Report &report)
{
    backup::Fields fields(value, "webui", report);
    String user;
    String password;
    const bool has_user = fields.text("user", 32, user);
    const bool has_password = fields.text("password", 64, password);
    fields.finish();
    if (!has_user && !has_password) {
        return;
    }
    if (!has_user || !has_password) {
        fields.problem(has_user ? "password" : "user", "user and password go together");
        return;
    }
    char error[64] = {};
    if (!px::webserver::import_login(user.c_str(), password.c_str(), apply, error, sizeof(error))) {
        // "user: ..." and "password: ..." name their field
        const std::string_view text = error;
        const size_t colon = text.find(": ");
        const std::string_view key = colon == std::string_view::npos ? "" : text.substr(0, colon);
        if (key == "user" || key == "password") {
            fields.problem(key, text.substr(colon + 2));
        } else {
            report.problem("webui", text);
        }
        return;
    }
    report.note("webui", apply ? "new login, sign in again" : "the login will be replaced, sign in again afterwards");
}

// ------------------------------------------------------------------------------------- leds

// The LEDs (docs/leds.md): the state, the power and timing config and the layout with its panels.
constexpr std::string_view kCorners[] = {"tl", "tr", "bl", "br"};
constexpr std::string_view kRuns[] = {"rows", "columns"};
constexpr std::string_view kDrivers[] = {"rmt", "parallel"};
constexpr std::string_view kOrders[] = {"grb", "rgb", "brg", "rbg", "gbr", "bgr"};
constexpr std::string_view kPowerOn[] = {"restore", "off", "on"};
constexpr std::string_view kOverlayModes[] = {"off", "text", "clock"};

void write_leds(backup::Writer &out)
{
    const px::leds::Status s = px::leds::status();
    const px::leds::Config c = px::leds::config();
    const px::leds::Layout l = px::leds::layout();
    out.section("leds");
    out.boolean("on", s.state.on);
    out.number("brightness", s.state.brightness);
    const px::effects::Info *effect = px::effects::info(s.state.effect);
    out.text("effect", effect != nullptr ? effect->slug : "solid");
    char color[8];
    std::snprintf(color, sizeof(color), "#%02x%02x%02x", s.state.color.r, s.state.color.g, s.state.color.b);
    out.text("color", color);
    out.number("speed", s.state.speed);
    const px::leds::AutoOff a = px::leds::auto_off();
    out.boolean("auto_off", a.enabled);
    out.number("auto_off_minutes", a.minutes);
    out.number("power_limit_mw", static_cast<long>(c.power_limit_mw));
    out.number("channel_mw", c.channel_mw);
    out.number("idle_mw", c.idle_mw);
    out.number("fps", c.fps);
    out.number("fade_ms", c.fade_ms);
    out.number("settle_ms", c.settle_ms);
    out.number("crossfade_ms", c.crossfade_ms);
    out.text("power_on", kPowerOn[static_cast<size_t>(c.power_on) % std::size(kPowerOn)]);
    out.text("driver", kDrivers[c.driver == px::output::Driver::Parallel ? 1 : 0]);
    out.boolean("dither", c.dither);
    const px::leds::Overlay o = px::leds::overlay();
    out.open("overlay");
    out.text("mode", kOverlayModes[static_cast<size_t>(o.mode) % std::size(kOverlayModes)], 2);
    out.text("text", o.text, 2);
    char overlay_color[8];
    std::snprintf(overlay_color, sizeof(overlay_color), "#%02x%02x%02x", o.color.r, o.color.g, o.color.b);
    out.text("color", overlay_color, 2);
    out.number("speed", o.speed, 2);
    out.boolean("clock_24h", o.clock_24h, 2);
    out.number("width", l.width);
    out.number("height", l.height);
    const px::leds::Lanes lanes = px::leds::lanes();
    out.open("lanes");
    for (size_t i = 0; i < s.available_lanes; i++) {
        const px::leds::LaneSettings &ls = lanes.lane[i];
        char lane[4];
        std::snprintf(lane, sizeof(lane), "%u", static_cast<unsigned>(i + 1));
        out.item("lane", lane);
        out.text("order", kOrders[static_cast<size_t>(ls.order)], 3);
        out.number("level", ls.level, 3);
        out.number("red", ls.red, 3);
        out.number("green", ls.green, 3);
        out.number("blue", ls.blue, 3);
        out.boolean("dither", ls.dither, 3);
    }
    // each animation's rotation and sound switch, the ones not at the default
    const px::leds::Styles styles = px::leds::styles();
    bool opened = false;
    for (size_t i = 0; i < px::effects::count() && i < px::leds::kMaxEffects; i++) {
        const px::leds::Style &st = styles.style[i];
        const px::leds::Style standard{};
        if (st.rotation == 0 && !st.reactive && st.strength == standard.strength && st.attack_ms == standard.attack_ms &&
            st.release_ms == standard.release_ms && st.palette == standard.palette) {
            continue;
        }
        if (!opened) {
            out.open("animations");
            opened = true;
        }
        out.item("animation", px::effects::info(i)->slug);
        out.number("rotation", st.rotation * 90, 3);
        out.boolean("reactive", st.reactive, 3);
        out.number("strength", st.strength, 3);
        out.number("attack_ms", st.attack_ms, 3);
        out.number("release_ms", st.release_ms, 3);
        out.text("palette", px::effects::palette_info(st.palette)->slug, 3);
    }
    out.open("panels");
    for (size_t i = 0; i < l.panel_count; i++) {
        const px::layout::Panel &p = l.panels[i];
        char lane[4];
        std::snprintf(lane, sizeof(lane), "%u", p.lane + 1u);
        out.item("lane", lane);
        out.number("x", p.x, 3);
        out.number("y", p.y, 3);
        out.number("width", p.width, 3);
        out.number("height", p.height, 3);
        out.text("first", kCorners[static_cast<size_t>(p.first)], 3);
        out.text("runs", kRuns[p.columns ? 1 : 0], 3);
        out.boolean("zigzag", p.zigzag, 3);
    }
}

// the palette of an animation or a preset by its slug; false (a problem) when there is none
bool read_palette(backup::Fields &fields, uint8_t &palette)
{
    String slug;
    if (!fields.text("palette", 32, slug)) {
        return true;
    }
    const size_t index = px::effects::find_palette(slug.c_str());
    if (index >= px::effects::palette_count()) {
        fields.problem("palette", "not a palette of this firmware");
        return false;
    }
    palette = static_cast<uint8_t>(index);
    return true;
}

bool parse_color(const String &text, px::effects::Rgb &out)
{
    if (text.size() != 7 || text[0] != '#') {
        return false;
    }
    char *end = nullptr;
    const unsigned long value = std::strtoul(text.c_str() + 1, &end, 16);
    if (*end != 0) {
        return false;
    }
    out = {static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
    return true;
}

void read_leds(const Node &value, bool apply, backup::Report &report)
{
    namespace leds = px::leds;
    const size_t problems = report.problems.size();
    const leds::Status s = leds::status();
    leds::State state = s.state;
    leds::Config config = leds::config();
    leds::Layout layout = leds::layout();
    backup::Fields fields(value, "leds", report);
    fields.boolean("on", state.on);
    fields.number("brightness", 0, 255, state.brightness);
    String effect;
    if (fields.text("effect", 32, effect)) {
        const size_t index = px::effects::find(effect.c_str());
        if (index >= px::effects::count()) {
            fields.problem("effect", "not an animation of this firmware");
        } else {
            state.effect = static_cast<uint8_t>(index);
        }
    }
    String color;
    if (fields.text("color", 7, color) && !parse_color(color, state.color)) {
        fields.problem("color", "#rrggbb");
    }
    fields.number("speed", 0, 255, state.speed);
    leds::AutoOff auto_off = leds::auto_off();
    const bool has_auto_off = fields.boolean("auto_off", auto_off.enabled);
    const bool has_minutes = fields.number("auto_off_minutes", 1, leds::kMaxAutoOffMinutes, auto_off.minutes);
    fields.number("power_limit_mw", 0, 10000000, config.power_limit_mw);
    fields.number("channel_mw", 1, 1000, config.channel_mw);
    fields.number("idle_mw", 0, 100, config.idle_mw);
    fields.number("fps", 1, 200, config.fps);
    fields.number("fade_ms", 0, 10000, config.fade_ms);
    fields.number("settle_ms", 0, 2000, config.settle_ms);
    fields.number("crossfade_ms", 0, leds::kMaxCrossfadeMs, config.crossfade_ms);
    size_t power_on = static_cast<size_t>(config.power_on);
    if (fields.choice("power_on", kPowerOn, power_on)) {
        config.power_on = static_cast<leds::PowerOn>(power_on);
    }
    fields.boolean("dither", config.dither);
    leds::Overlay overlay = leds::overlay();
    const Node *overlay_node = fields.node("overlay");
    if (overlay_node != nullptr) {
        backup::Fields of(*overlay_node, fields.path("overlay"), report);
        size_t mode = static_cast<size_t>(overlay.mode);
        if (of.choice("mode", kOverlayModes, mode)) {
            overlay.mode = static_cast<leds::OverlayMode>(mode);
        }
        String text(overlay.text);
        if (of.text("text", leds::kOverlayTextSize - 1, text)) {
            std::snprintf(overlay.text, sizeof(overlay.text), "%s", text.c_str());
        }
        String overlay_color;
        if (of.text("color", 7, overlay_color) && !parse_color(overlay_color, overlay.color)) {
            of.problem("color", "#rrggbb");
        }
        of.number("speed", 0, 255, overlay.speed);
        of.boolean("clock_24h", overlay.clock_24h);
        of.finish();
    }
    size_t driver = config.driver == px::output::Driver::Parallel ? 1 : 0;
    if (fields.choice("driver", kDrivers, driver)) {
        config.driver = driver == 1 ? px::output::Driver::Parallel : px::output::Driver::Rmt;
    }
    const bool has_width = fields.number("width", 0, 8192, layout.width);
    const bool has_height = fields.number("height", 0, 8192, layout.height);
    leds::Lanes lanes = leds::lanes();
    const Node *lane_list = fields.node("lanes");
    if (lane_list != nullptr && lane_list->kind != Kind::List) {
        fields.problem("lanes", "a list of lanes expected (- lane: 1 ...)");
    } else if (lane_list != nullptr) {
        for (const Node &item : lane_list->list) {
            backup::Fields lf(item, fields.path("lanes"), report);
            long number = 0;
            if (!lf.number("lane", 1, static_cast<long>(px::output::kMaxLanes), number)) {
                lf.problem("lane", "the number of the lane is missing");
                lf.finish();
                continue;
            }
            leds::LaneSettings &ls = lanes.lane[number - 1];
            size_t order = static_cast<size_t>(ls.order);
            lf.choice("order", kOrders, order);
            ls.order = static_cast<px::output::ColorOrder>(order);
            lf.number("level", 0, 255, ls.level);
            lf.number("red", 0, 255, ls.red);
            lf.number("green", 0, 255, ls.green);
            lf.number("blue", 0, 255, ls.blue);
            lf.boolean("dither", ls.dither);
            lf.finish();
        }
    }
    leds::Styles styles = leds::styles();
    const Node *style_list = fields.node("animations");
    if (style_list != nullptr && style_list->kind != Kind::List) {
        fields.problem("animations", "a list of animations expected (- animation: fire ...)");
    } else if (style_list != nullptr) {
        // the list is complete: an animation that is not in it gets the default
        styles = {};
        for (const Node &item : style_list->list) {
            backup::Fields af(item, fields.path("animations"), report);
            String slug;
            const size_t index = af.text("animation", 32, slug) ? px::effects::find(slug.c_str()) : px::effects::count();
            if (index >= px::effects::count() || index >= leds::kMaxEffects) {
                af.problem("animation", "not an animation of this firmware");
                af.finish();
                continue;
            }
            long degrees = 0;
            af.number("rotation", 0, 270, degrees);
            if (degrees % 90 != 0) {
                af.problem("rotation", "0, 90, 180 or 270");
            }
            styles.style[index].rotation = static_cast<uint8_t>(degrees / 90);
            af.boolean("reactive", styles.style[index].reactive);
            af.number("strength", 0, 255, styles.style[index].strength);
            af.number("attack_ms", 0, px::effects::kMaxAttackMs, styles.style[index].attack_ms);
            af.number("release_ms", 0, px::effects::kMaxReleaseMs, styles.style[index].release_ms);
            read_palette(af, styles.style[index].palette);
            af.finish();
        }
    }
    const Node *list = fields.node("panels");
    const bool has_panels = list != nullptr;
    if (list != nullptr && list->kind != Kind::List) {
        fields.problem("panels", "a list of panels expected (- lane: 1 ...)");
    } else if (list != nullptr && list->list.size() > leds::kMaxPanels) {
        fields.problem("panels", "at most 16 panels");
    } else if (list != nullptr) {
        layout.panel_count = 0;
        for (const Node &item : list->list) {
            backup::Fields panel(item, fields.path("panels"), report);
            px::layout::Panel p{};
            long lane = 1;
            size_t first = 0, runs = 0;
            panel.number("lane", 1, 16, lane);
            panel.number("x", 0, 8192, p.x);
            panel.number("y", 0, 8192, p.y);
            panel.number("width", 1, 8192, p.width);
            panel.number("height", 1, 8192, p.height);
            panel.choice("first", kCorners, first);
            panel.choice("runs", kRuns, runs);
            panel.boolean("zigzag", p.zigzag);
            panel.finish();
            p.lane = static_cast<uint8_t>(lane - 1);
            p.first = static_cast<px::layout::Corner>(first);
            p.columns = runs == 1;
            layout.panels[layout.panel_count++] = p;
        }
    }
    fields.finish();
    if (report.problems.size() != problems) {
        return;
    }
    if (has_width || has_height || has_panels) {
        // the layout is checked like the web UI's: one that cannot be shown is a problem
        if (layout.panel_count > 0) {
            auto table = px::layout::build(layout.width, layout.height, {layout.panels, layout.panel_count});
            if (!table) {
                report.problem("leds.panels", table.error().text);
                return;
            }
        }
        if (apply) {
            char error[96] = {};
            if (!leds::set_layout(layout, error, sizeof(error))) {
                report.problem("leds", error);
                return;
            }
        }
    }
    if (apply) {
        char error[64] = {};
        if (!leds::set_config(config, error, sizeof(error))) {
            report.problem("leds", error);
            return;
        }
        if (lane_list != nullptr) {
            leds::set_lanes(lanes);
        }
        if (overlay_node != nullptr) {
            leds::set_overlay(overlay);
        }
        if (style_list != nullptr) {
            for (size_t i = 0; i < px::effects::count() && i < leds::kMaxEffects; i++) {
                leds::set_style(i, styles.style[i]);
            }
        }
        if (has_auto_off || has_minutes) {
            leds::set_auto_off(auto_off);
        }
        leds::set_effect(state.effect);
        leds::set_color(state.color);
        leds::set_speed(state.speed);
        leds::set_brightness(state.brightness);
        leds::set_on(state.on);
    }
}

// ---------------------------------------------------------------------------------- presets

// The presets (docs/leds.md section 10), the backup's presets part: the whole list, in its order.
void write_presets(backup::Writer &out)
{
    out.section("presets");
    out.open("list");
    for (size_t i = 0; i < px::presets::count(); i++) {
        px::presets::Preset p{};
        if (!px::presets::get(i, p)) {
            break;
        }
        out.item("name", p.name);
        const px::effects::Info *effect = px::effects::info(p.effect);
        out.text("effect", effect != nullptr ? effect->slug : "solid", 3);
        char color[8];
        std::snprintf(color, sizeof(color), "#%02x%02x%02x", p.color.r, p.color.g, p.color.b);
        out.text("color", color, 3);
        out.number("speed", p.speed, 3);
        out.number("brightness", p.brightness, 3);
        out.number("rotation", p.style.rotation * 90, 3);
        out.boolean("reactive", p.style.reactive, 3);
        out.number("strength", p.style.strength, 3);
        out.number("attack_ms", p.style.attack_ms, 3);
        out.number("release_ms", p.style.release_ms, 3);
        out.text("palette", px::effects::palette_info(p.style.palette)->slug, 3);
    }
}

void read_presets(const Node &value, bool apply, backup::Report &report)
{
    namespace presets = px::presets;
    // the web server's task is the only caller; too large for its stack
    static presets::Preset list[presets::kMaxPresets];
    const size_t problems = report.problems.size();
    backup::Fields fields(value, "presets", report);
    const Node *items = fields.node("list");
    fields.finish();
    size_t count = 0;
    if (items != nullptr && items->kind != Kind::List) {
        fields.problem("list", "a list of presets expected (- name: ...)");
    } else if (items != nullptr && items->list.size() > presets::kMaxPresets) {
        fields.problem("list", "at most 16 presets");
    } else if (items != nullptr) {
        for (const Node &item : items->list) {
            backup::Fields pf(item, fields.path("list"), report);
            presets::Preset &p = list[count];
            p = {};
            p.speed = 128;
            p.brightness = 128;
            p.color = {255, 120, 40};
            String name;
            if (!pf.text("name", presets::kNameSize - 1, name) || name.empty()) {
                pf.problem("name", "the name of the preset is missing");
            }
            std::snprintf(p.name, sizeof(p.name), "%s", name.c_str());
            String effect;
            const size_t index = pf.text("effect", 32, effect) ? px::effects::find(effect.c_str()) : 0;
            if (index >= px::effects::count() || index >= px::leds::kMaxEffects) {
                pf.problem("effect", "not an animation of this firmware");
            }
            p.effect = static_cast<uint8_t>(index);
            String color;
            if (pf.text("color", 7, color) && !parse_color(color, p.color)) {
                pf.problem("color", "#rrggbb");
            }
            pf.number("speed", 0, 255, p.speed);
            pf.number("brightness", 1, 255, p.brightness);
            long degrees = 0;
            pf.number("rotation", 0, 270, degrees);
            if (degrees % 90 != 0) {
                pf.problem("rotation", "0, 90, 180 or 270");
            }
            p.style.rotation = static_cast<uint8_t>(degrees / 90);
            pf.boolean("reactive", p.style.reactive);
            pf.number("strength", 0, 255, p.style.strength);
            pf.number("attack_ms", 0, px::effects::kMaxAttackMs, p.style.attack_ms);
            pf.number("release_ms", 0, px::effects::kMaxReleaseMs, p.style.release_ms);
            read_palette(pf, p.style.palette);
            pf.finish();
            const bool twice = std::any_of(list, list + count, [&](const presets::Preset &other) {
                return std::strcmp(other.name, p.name) == 0;
            });
            if (twice) {
                fields.problem("list", text_of("'%s' is in the list twice", name.c_str()));
            }
            count++;
        }
    }
    if (report.problems.size() != problems || items == nullptr || !apply) {
        return;
    }
    char error[64] = {};
    if (!presets::set_all(list, count, error, sizeof(error))) {
        report.problem("presets", error);
    }
}

// ------------------------------------------------------------------------------------ input

// What the buttons and the encoder's push do (docs/leds.md section 9): the buttons the board has
constexpr std::string_view kInputKeys[] = {"click", "double", "hold"};

void write_actions(backup::Writer &out, const px::input::Actions &a, int level)
{
    out.text("click", px::input::action_slug(a.click), level);
    out.text("double", px::input::action_slug(a.double_click), level);
    out.text("hold", px::input::action_slug(a.hold), level);
}

void write_input(backup::Writer &out)
{
    const px::input::Status s = px::input::status();
    const px::input::Settings c = px::input::settings();
    out.section("input");
    out.open("buttons");
    for (size_t i = 0; i < px::input::kMaxButtons; i++) {
        if (s.button_gpio[i] < 0) {
            continue;
        }
        char number[4];
        std::snprintf(number, sizeof(number), "%u", static_cast<unsigned>(i + 1));
        out.item("button", number);
        write_actions(out, c.button[i], 3);
    }
    if (s.encoder_button >= 0) {
        out.open("encoder");
        write_actions(out, c.button[px::input::kEncoderButton], 2);
    }
}

// one button's actions; a slug that is no action is a problem
void read_actions(backup::Fields &fields, px::input::Actions &a)
{
    px::input::Action *actions[] = {&a.click, &a.double_click, &a.hold};
    for (size_t i = 0; i < std::size(kInputKeys); i++) {
        String slug;
        if (fields.text(kInputKeys[i], 32, slug) && !px::input::find_action(slug.c_str(), *actions[i])) {
            fields.problem(kInputKeys[i], "not an action (none, on-off, next-animation, ...)");
        }
    }
}

void read_input(const Node &value, bool apply, backup::Report &report)
{
    const size_t problems = report.problems.size();
    px::input::Settings c = px::input::settings();
    backup::Fields fields(value, "input", report);
    const Node *list = fields.node("buttons");
    if (list != nullptr && list->kind != Kind::List) {
        fields.problem("buttons", "a list of buttons expected (- button: 1 ...)");
    } else if (list != nullptr) {
        for (const Node &item : list->list) {
            backup::Fields bf(item, fields.path("buttons"), report);
            long number = 0;
            if (!bf.number("button", 1, static_cast<long>(px::input::kMaxButtons), number)) {
                bf.problem("button", "the number of the button is missing");
                bf.finish();
                continue;
            }
            read_actions(bf, c.button[number - 1]);
            bf.finish();
        }
    }
    if (const Node *encoder = fields.node("encoder"); encoder != nullptr) {
        backup::Fields ef(*encoder, fields.path("encoder"), report);
        read_actions(ef, c.button[px::input::kEncoderButton]);
        ef.finish();
    }
    fields.finish();
    if (report.problems.size() != problems || !apply) {
        return;
    }
    char error[64] = {};
    if (!px::input::set_settings(c, error, sizeof(error))) {
        report.problem("input", error);
    }
}

// ------------------------------------------------------------------------------------ audio

// The audio input (docs/leds.md section 6.1): the microphone's settings and the UDP inputs
void write_audio(backup::Writer &out)
{
    if (!px::audio::running()) {
        return;
    }
    const px::audio::Settings c = px::audio::settings();
    const px::audio::Inputs in = px::audio::inputs();
    out.section("audio");
    out.boolean("warls", in.warls);
    out.number("warls_port", in.warls_port);
    out.boolean("wled_sync", in.wled_sync);
    out.number("gain_db", c.gain_db);
    out.boolean("agc", c.agc);
    out.number("silence_db", c.silence_db);
    out.number("range_db", c.range_db);
    out.number("beat_ratio_tenths", c.beat_ratio_tenths);
}

void read_audio(const Node &value, bool apply, backup::Report &report)
{
    namespace audio = px::audio;
    const size_t problems = report.problems.size();
    audio::Settings c = audio::settings();
    audio::Inputs in = audio::inputs();
    backup::Fields fields(value, "audio", report);
    const bool has_warls = fields.boolean("warls", in.warls);
    const bool has_port = fields.number("warls_port", 1, 65535, in.warls_port);
    const bool has_sync = fields.boolean("wled_sync", in.wled_sync);
    fields.number("gain_db", audio::kMinGainDb, audio::kMaxGainDb, c.gain_db);
    fields.boolean("agc", c.agc);
    fields.number("silence_db", audio::kMinSilenceDb, audio::kMaxSilenceDb, c.silence_db);
    fields.number("range_db", audio::kMinRangeDb, audio::kMaxRangeDb, c.range_db);
    fields.number("beat_ratio_tenths", audio::kMinBeatTenths, audio::kMaxBeatTenths, c.beat_ratio_tenths);
    fields.finish();
    if (report.problems.size() != problems || !apply) {
        return;
    }
    char error[64] = {};
    if (!audio::set_settings(c, error, sizeof(error))) {
        report.problem("audio", error);
    }
    if (has_warls || has_port || has_sync) {
        audio::set_inputs(in);
    }
}

// ------------------------------------------------------------------------------------- mqtt

// MQTT (docs/leds.md section 8): the broker and the topics; the password never, one added by hand
// is used
void write_mqtt(backup::Writer &out)
{
    const px::hass::Settings s = px::hass::settings();
    out.section("mqtt");
    out.boolean("enabled", s.enabled);
    out.text("uri", s.uri);
    out.text("username", s.username);
    if (px::hass::password_set()) {
        out.comment("password: stored on the device, not exported; add it here to set it");
    }
    out.text("prefix", s.prefix);
    out.text("base", s.base);
}

void read_mqtt(const Node &value, bool apply, backup::Report &report)
{
    const size_t problems = report.problems.size();
    px::hass::Settings s = px::hass::settings();
    backup::Fields fields(value, "mqtt", report);
    String uri(s.uri), username(s.username), prefix(s.prefix), base(s.base), password;
    fields.boolean("enabled", s.enabled);
    fields.text("uri", px::hass::kUriSize - 1, uri);
    fields.text("username", px::hass::kUserSize - 1, username);
    const bool has_password = fields.text("password", px::hass::kPasswordSize - 1, password);
    fields.text("prefix", px::hass::kTopicSize - 1, prefix);
    fields.text("base", px::hass::kTopicSize - 1, base);
    fields.finish();
    if (report.problems.size() != problems) {
        return;
    }
    std::snprintf(s.uri, sizeof(s.uri), "%s", uri.c_str());
    std::snprintf(s.username, sizeof(s.username), "%s", username.c_str());
    std::snprintf(s.prefix, sizeof(s.prefix), "%s", prefix.c_str());
    std::snprintf(s.base, sizeof(s.base), "%s", base.c_str());
    if (s.enabled && !has_password && !px::hass::password_set() && !username.empty()) {
        report.note("mqtt", "the broker's password is not in the file: add it on the MQTT page");
    }
    if (!apply) {
        return;
    }
    char error[96] = {};
    if (!px::hass::set_settings(s, has_password ? password.c_str() : nullptr, error, sizeof(error))) {
        report.problem("mqtt", error);
    }
}

// ------------------------------------------------------------------------------------- time

constexpr std::string_view kServerKeys[px::platform::kTimeServers] = {"server_1", "server_2"};

// The clock (docs/ui/web/web-app.md section 4): the time zone (an IANA name or a POSIX TZ rule)
// and the SNTP servers, empty for none (both empty: the defaults)
void write_time(backup::Writer &out)
{
    char zone[px::platform::kTimeZoneSize] = {};
    px::platform::time_zone(zone, sizeof(zone));
    const px::platform::TimeServers servers = px::platform::time_servers();
    out.section("time");
    out.text("zone", zone);
    for (size_t i = 0; i < px::platform::kTimeServers; ++i) {
        out.text(kServerKeys[i], servers.names[i]);
    }
}

void read_time(const Node &value, bool apply, backup::Report &report)
{
    const size_t problems = report.problems.size();
    char current[px::platform::kTimeZoneSize] = {};
    px::platform::time_zone(current, sizeof(current));
    px::platform::TimeServers servers = px::platform::time_servers();
    backup::Fields fields(value, "time", report);
    String zone(current);
    if (fields.text("zone", px::platform::kTimeZoneSize - 1, zone) && !px::platform::valid_time_zone(zone.c_str())) {
        fields.problem("zone", "neither a time zone this firmware knows nor a POSIX TZ rule");
    }
    for (size_t i = 0; i < px::platform::kTimeServers; ++i) {
        String name(servers.names[i]);
        if (fields.text(kServerKeys[i], px::platform::kTimeServerSize - 1, name)) {
            std::snprintf(servers.names[i], sizeof(servers.names[i]), "%s", name.c_str());
        }
    }
    fields.finish();
    char error[96] = {};
    if (report.problems.size() == problems && !px::platform::check_time_servers(servers, error, sizeof(error))) {
        report.problem("time", error);
    }
    if (report.problems.size() != problems || !apply) {
        return;
    }
    px::platform::set_time_zone(zone.c_str());
    if (!px::platform::set_time_servers(servers, error, sizeof(error))) {
        report.problem("time", error);
    }
}

// ------------------------------------------------------------------------------------ table

// in this order: a section may depend on one before it
const backup::Section kSections[] = {
    {"network", write_network, read_network},
    {"leds", write_leds, read_leds},
    {"audio", write_audio, read_audio},
    {"mqtt", write_mqtt, read_mqtt},
    {"time", write_time, read_time},
    {"input", write_input, read_input, px::input::running},
    {"webui", write_webui, read_webui},
    {"presets", write_presets, read_presets, nullptr, backup::Part::Presets},
};

const px::webserver::BackupAccess kBackupAccess{
    .export_file =
        [](bool settings, bool presets) {
            const backup::Device device{px::board::info().name, esp_app_get_description()->version};
            return backup::write(device, kSections, settings, presets);
        },
    .import_file =
        [](std::string_view text, bool check_only) {
            backup::Report report = backup::read(text, kSections, !check_only);
            px::webserver::BackupReport out;
            out.problems = std::move(report.problems);
            out.ignored = std::move(report.ignored);
            out.notes = std::move(report.notes);
            out.sections = std::move(report.sections);
            return out;
        },
};

} // namespace

const px::webserver::BackupAccess &backup_access()
{
    return kBackupAccess;
}
