// The live socket of the web app (docs/ui/web/web-app.md section 4.1), login required after the
// setup:
//
//   GET /api/live   WebSocket. The browser sends a text frame with the paths of the GETs it shows,
//                   up to kMaxFollow separated by one space ("/api/leds /api/audio /api/presets");
//                   the server sends {"path": one of them, "state": that GET's answer} as a text
//                   frame for each at once and again whenever an answer changes. An empty frame
//                   stops them all (a hidden tab).
//
// One socket per tab instead of a page that asks every few seconds: nothing to answer while nothing
// changes, and one session instead of the browser's pool of polling connections. Every
// `period_ms` of a topic its answer is built again and sent when its hash differs from the last
// one sent to that tab. A session whose login ended (logout, "log out everywhere", a new password,
// expiry) is closed. Everything here runs on the server's task: the handler, the close callback and
// the tick (httpd_queue_work), so nothing is locked; the topics' writers run there as well. While a
// tab follows a page (a path, not a hidden tab) the Wi-Fi stays awake (network::AwakeHold::Web,
// docs/architecture.md section 9.2).

#include <cstring>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "px/network/network.hpp"

#include "auth.hpp"
#include "http_util.hpp"

namespace px::webserver::detail {

// The recovery app has no WebSocket support (sdkconfig.recovery): no /api/live there, the web app
// falls back to asking (live() in web/app.js).
#if CONFIG_HTTPD_WS_SUPPORT
#if !CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT
#error "the live socket checks the login before the handshake: CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT"
#endif

namespace {

constexpr const char *kTag = "web";
constexpr size_t kMaxTopics = 12;
// a tab each; more than the browsers of a household
constexpr size_t kMaxClients = 8;
// paths one tab follows at once (Control: the light and the presets)
constexpr size_t kMaxFollow = 3;
constexpr size_t kMaxPath = 32;
constexpr size_t kMaxFrame = kMaxFollow * kMaxPath;
constexpr uint64_t kTickUs = 1000000;
// `{}` after this long without a frame: the browser sends nothing while it follows a page, so
// this is how it finds a socket that died (the device restarted, the laptop slept); the web app
// opens a new one after 25 s without a frame
constexpr int64_t kHeartbeatMs = 10000;

struct Topic {
    const char *path;
    uint32_t period_ms;
    StateWriter write;
};

struct Follow {
    // into g_topics, -1: none
    int topic = -1;
    uint32_t hash = 0;
    int64_t due = 0;
};

struct Client {
    bool used = false;
    // the close could not be queued (the server's queue was full): tried again at the next tick
    bool closing = false;
    int fd = 0;
    Follow follows[kMaxFollow];
    // the last frame sent (ms): the heartbeat follows kHeartbeatMs after it
    int64_t sent = 0;
    // the session's token at the handshake, empty in setup mode
    char token[auth::kTokenSize] = {};
};

Topic g_topics[kMaxTopics];
size_t g_topic_count = 0;
// In PSRAM on the S3 (the server's task only). That section starts as zeros, not with the
// defaults: a slot is read only once it is used, and accept_client assigns it in full first.
EXT_RAM_BSS_ATTR Client g_clients[kMaxClients];
httpd_handle_t g_server = nullptr;

int64_t now_ms()
{
    return esp_timer_get_time() / 1000;
}

uint32_t fnv1a(const char *data, size_t length)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < length; i++) {
        hash = (hash ^ static_cast<uint8_t>(data[i])) * 16777619u;
    }
    return hash;
}

bool following(const Client &client)
{
    for (const Follow &follow : client.follows) {
        if (follow.topic >= 0) {
            return true;
        }
    }
    return false;
}

Client *find(int fd)
{
    for (Client &client : g_clients) {
        if (client.used && client.fd == fd) {
            return &client;
        }
    }
    return nullptr;
}

Client *free_slot()
{
    for (Client &client : g_clients) {
        if (!client.used) {
            return &client;
        }
    }
    return nullptr;
}

// the Wi-Fi's hold while any tab follows a page
void update_web_hold()
{
    static bool held = false;
    bool any = false;
    for (const Client &client : g_clients) {
        any = any || (client.used && following(client));
    }
    if (any != held) {
        network::hold_awake(network::AwakeHold::Web, any);
        held = any;
    }
}

// The close is queued on the server's task; with its queue full (the previews send 20 times a
// second) the client stays, without its follows, and the next tick tries again.
void drop(Client &client)
{
    if (httpd_sess_trigger_close(g_server, client.fd) == ESP_OK) {
        client = Client{};
    } else {
        client.closing = true;
        for (Follow &follow : client.follows) {
            follow = Follow{};
        }
    }
    update_web_hold();
}

// false when the frame could not be sent: the session is closed then
bool send_text(Client &client, const char *text, size_t length, int64_t now)
{
    httpd_ws_frame_t frame{};
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = reinterpret_cast<uint8_t *>(const_cast<char *>(text));
    frame.len = length;
    if (httpd_ws_send_frame_async(g_server, client.fd, &frame) != ESP_OK) {
        drop(client);
        return false;
    }
    client.sent = now;
    return true;
}

// the follows of `client` that are due; false when the session was closed
bool send_due(Client &client, int64_t now, memory::Vector<char> &buffer)
{
    for (Follow &follow : client.follows) {
        if (follow.topic < 0 || now < follow.due) {
            continue;
        }
        const Topic &topic = g_topics[follow.topic];
        follow.due = now + topic.period_ms;
        const size_t length = build_state(topic.write, buffer);
        const uint32_t hash = length > 0 ? fnv1a(buffer.data(), length) : follow.hash;
        if (hash == follow.hash) {
            continue;
        }
        // {"path": ..., "state": <the GET's answer>}: the page tells its paths apart, and a frame
        // still on its way when the page changed is not taken for the next page's
        memory::String text = "{\"path\":\"";
        text.append(topic.path).append("\",\"state\":").append(buffer.data(), length).append("}");
        if (!send_text(client, text.data(), text.size(), now)) {
            return false;
        }
        follow.hash = hash;
    }
    return true;
}

void tick(void *)
{
    const int64_t now = now_ms();
    memory::Vector<char> buffer;
    for (Client &client : g_clients) {
        if (!client.used) {
            continue;
        }
        if (client.closing) {
            drop(client);
            continue;
        }
        char user[auth::kUsernameSize];
        if (auth::has_user() && !auth::check_token(client.token, user)) {
            ESP_LOGD(kTag, "live %d: the login ended", client.fd);
            drop(client);
            continue;
        }
        if (!send_due(client, now, buffer)) {
            continue;
        }
        if (now - client.sent >= kHeartbeatMs) {
            send_text(client, "{}", 2, now);
        }
    }
}

// Before the handshake, the last call with the request's headers (ESP-IDF does not call the
// handler for the handshake itself): no WebSocket without the login (401 then), and the client
// with its session's token. A handshake that fails after this closes the session, and
// close_session forgets the client.
esp_err_t accept_client(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_FAIL;
    }
    Client *client = free_slot();
    if (client == nullptr) {
        ESP_LOGW(kTag, "live: %u sockets open already", static_cast<unsigned>(kMaxClients));
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "too many live sockets\n");
        return ESP_FAIL;
    }
    *client = Client{};
    client->used = true;
    client->fd = httpd_req_to_sockfd(req);
    client->sent = now_ms();
    if (!session_token(req, client->token)) {
        client->token[0] = 0;
    }
    return ESP_OK;
}

// the topic of `path` (`length` characters), -1: none
int topic_of(const char *path, size_t length)
{
    for (size_t i = 0; i < g_topic_count; i++) {
        if (std::strlen(g_topics[i].path) == length && std::strncmp(g_topics[i].path, path, length) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// a frame of the browser: the paths it follows now
esp_err_t handle_live(httpd_req_t *req)
{
    const int fd = httpd_req_to_sockfd(req);
    httpd_ws_frame_t frame{};
    if (httpd_ws_recv_frame(req, &frame, 0) != ESP_OK || frame.len >= kMaxFrame) {
        return ESP_FAIL;
    }
    char paths[kMaxFrame] = {};
    frame.payload = reinterpret_cast<uint8_t *>(paths);
    if (frame.len > 0 && httpd_ws_recv_frame(req, &frame, sizeof(paths) - 1) != ESP_OK) {
        return ESP_FAIL;
    }
    Client *client = find(fd);
    // a failed handler closes the session: the way out for one whose close could not be queued
    if (client == nullptr || client->closing || frame.type != HTTPD_WS_TYPE_TEXT) {
        return ESP_FAIL;
    }
    for (Follow &follow : client->follows) {
        follow = Follow{};
    }
    size_t count = 0;
    for (const char *path = paths; *path != 0 && count < kMaxFollow;) {
        const size_t length = std::strcspn(path, " ");
        const int topic = topic_of(path, length);
        bool taken = false;
        for (size_t i = 0; i < count; i++) {
            taken = taken || client->follows[i].topic == topic;
        }
        if (topic >= 0 && !taken) {
            // the answer at once (hash 0, due now), the page draws from it
            client->follows[count++].topic = topic;
        } else if (topic < 0 && length > 0) {
            ESP_LOGD(kTag, "live %d: no topic %.*s", fd, static_cast<int>(length), path);
        }
        path += length;
        path += *path == ' ' ? 1 : 0;
    }
    update_web_hold();
    if (count > 0) {
        httpd_queue_work(g_server, tick, nullptr);
    }
    return ESP_OK;
}

} // namespace

void add_live_topic(const char *path, uint32_t period_ms, StateWriter write)
{
    if (g_topic_count < kMaxTopics && std::strlen(path) < kMaxPath) {
        g_topics[g_topic_count++] = {path, period_ms, write};
    } else {
        ESP_LOGE(kTag, "live: no room for %s", path);
    }
}

void register_live_routes(httpd_handle_t server)
{
    g_server = server;
    httpd_uri_t live = route("/api/live", HTTP_GET, handle_live);
    live.is_websocket = true;
    live.ws_pre_handshake_cb = accept_client;
    httpd_register_uri_handler(server, &live);

    const esp_timer_create_args_t args = {
        .callback = [](void *) { httpd_queue_work(g_server, tick, nullptr); },
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "web_live",
        .skip_unhandled_events = true,
    };
    esp_timer_handle_t timer = nullptr;
    if (esp_timer_create(&args, &timer) != ESP_OK || esp_timer_start_periodic(timer, kTickUs) != ESP_OK) {
        ESP_LOGW(kTag, "live: no timer");
    }
}

void live_forget(int fd)
{
    if (Client *client = find(fd)) {
        *client = Client{};
        update_web_hold();
    }
}

void live_close_token(const char *token)
{
    for (Client &client : g_clients) {
        if (client.used && token[0] != 0 && std::strcmp(client.token, token) == 0) {
            drop(client);
        }
    }
}

#else

void add_live_topic(const char *, uint32_t, StateWriter) {}
void register_live_routes(httpd_handle_t) {}
void live_forget(int) {}
void live_close_token(const char *) {}

#endif // CONFIG_HTTPD_WS_SUPPORT

} // namespace px::webserver::detail
