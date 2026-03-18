#ifndef SIGAW_DISCORD_IPC_H
#define SIGAW_DISCORD_IPC_H

#include <algorithm>
#include <string>
#include <functional>
#include <vector>
#include <deque>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <fstream>
#include <filesystem>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>

#include <curl/curl.h>

#include "json_utils.h"

namespace sigaw {

enum class Opcode : uint32_t {
    Handshake = 0, Frame = 1, Close = 2, Ping = 3, Pong = 4,
};

struct VoiceUser {
    uint64_t    id = 0;
    std::string username;
    std::string avatar;
    bool speaking    = false;
    bool self_mute   = false;
    bool self_deaf   = false;
    bool server_mute = false;
    bool server_deaf = false;
};

struct VoiceChatMessage {
    uint64_t    id = 0;
    uint64_t    author_id = 0;
    uint64_t    observed_at_ms = 0;
    std::string author_name;
    std::string content;
};

struct VoiceState {
    std::string            channel_id;
    std::string            channel_name;
    std::string            guild_id;
    std::vector<VoiceUser> users;
    std::vector<VoiceChatMessage> chat_messages;
};

struct DiscordUserIdentity {
    uint64_t    id = 0;
    std::string username;
    std::string avatar;

    bool empty() const {
        return id == 0 && username.empty() && avatar.empty();
    }
};

class DiscordMessageInbox {
public:
    void observe(const json& message) {
        capture_local_user(message);
    }

    void store(const json& message) {
        observe(message);
        if (json_utils::string_or(message, "cmd") == "DISPATCH") {
            dispatches_.push_back(message);
        } else {
            responses_.push_back(message);
        }
    }

    bool store_or_match(const json& message,
                        const std::string& nonce,
                        const std::string& expected_cmd,
                        json& matched_response)
    {
        store(message);
        return take_response(nonce, expected_cmd, matched_response);
    }

    bool take_dispatch(json& message) {
        if (dispatches_.empty()) {
            return false;
        }

        message = dispatches_.front();
        dispatches_.pop_front();
        return true;
    }

    bool take_response(const std::string& nonce,
                       const std::string& expected_cmd,
                       json& message)
    {
        const auto it = std::find_if(responses_.begin(), responses_.end(),
            [&](const json& candidate) {
                if (json_utils::string_or(candidate, "nonce") != nonce) {
                    return false;
                }

                const auto cmd = json_utils::string_or(candidate, "cmd");
                return expected_cmd.empty() || cmd == expected_cmd;
            });

        if (it == responses_.end()) {
            return false;
        }

        message = *it;
        responses_.erase(it);
        return true;
    }

    const DiscordUserIdentity& local_user() const {
        return local_user_;
    }

private:
    void capture_local_user(const json& message) {
        const auto* data = json_utils::object_or_null(message, "data");
        if (!data) {
            return;
        }

        const auto* user = json_utils::object_or_null(*data, "user");
        if (!user) {
            return;
        }

        const uint64_t id = json_utils::u64_or(*user, "id");
        const std::string username = json_utils::display_name(*user);
        if (id == 0 && username.empty()) {
            return;
        }

        local_user_.id = id;
        local_user_.username = username;
        if (user->contains("avatar")) {
            local_user_.avatar = json_utils::string_or(*user, "avatar");
        }
    }

    std::deque<json>      dispatches_;
    std::deque<json>      responses_;
    DiscordUserIdentity   local_user_;
};

class DiscordIpc {
public:
    explicit DiscordIpc(const std::string& client_id)
        : client_id_(client_id) {}

    ~DiscordIpc() { disconnect(); }

    /* Connect to Discord's local IPC socket (tries 0..9). */
    bool connect() {
        const char* xrd = getenv("XDG_RUNTIME_DIR");
        std::string rd = xrd
            ? xrd
            : "/run/user/" + std::to_string(static_cast<unsigned long>(::getuid()));
        std::string fp = rd + "/app/com.discordapp.Discord";

        for (int i = 0; i <= 9; i++) {
            std::string s = std::to_string(i);
            if (try_sock(rd + "/discord-ipc-" + s)) return true;
            if (try_sock(fp + "/discord-ipc-" + s)) return true;
            if (try_sock("/tmp/discord-ipc-" + s))  return true;
        }
        fprintf(stderr, "[sigaw] Could not find Discord IPC socket\n");
        return false;
    }

    void disconnect() {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        connected_ = false;
        clear_auth_state();
        inbox_ = {};
    }

    /* Handshake: send version + client_id, get READY. */
    bool handshake() {
        json hs = {{"v", 1}, {"client_id", client_id_}};
        if (!send(Opcode::Handshake, hs.dump())) return false;

        Opcode op; std::string r;
        if (!recv(op, r)) return false;
        auto j = json::parse(r, nullptr, false);
        if (j.is_discarded()) return false;
        inbox_.observe(j);

        if (json_utils::string_or(j, "cmd") == "DISPATCH" &&
            json_utils::string_or(j, "evt") == "READY") {
            fprintf(stderr, "[sigaw] Handshake OK\n");
            if (j.contains("data") && j["data"].contains("user"))
                fprintf(stderr, "[sigaw] User: %s\n",
                        json_utils::string_or(j["data"]["user"], "username", "?").c_str());
            return true;
        }
        return false;
    }

    /* Full OAuth2 authorization + authentication flow.
     * 1) Check for cached token -- if valid, use it directly.
     * 2) Otherwise: AUTHORIZE -> exchange code -> AUTHENTICATE.
     */
    bool authorize(const std::string& secret, bool require_messages_read = false) {
        /* Try cached token first */
        std::string token = load_token();
        if (!token.empty()) {
            if (authenticate(token) && scope_satisfied(require_messages_read)) {
                return true;
            }

            if (authed_ && !scope_satisfied(require_messages_read)) {
                fprintf(stderr, "[sigaw] Cached token missing messages.read scope, re-authorizing\n");
            } else {
                fprintf(stderr, "[sigaw] Cached token expired, re-authorizing\n");
            }
            clear_auth_state();
        }

        /* Send AUTHORIZE request -- Discord shows popup to user */
        json scopes = json::array({"rpc", "rpc.voice.read"});
        if (require_messages_read) {
            scopes.push_back("messages.read");
        }
        json req = {
            {"cmd", "AUTHORIZE"},
            {"nonce", nonce()},
            {"args", {
                {"client_id", client_id_},
                {"scopes", scopes},
            }}
        };
        json j;
        /* This blocks until user clicks Authorize in Discord */
        if (!send_request(req, j)) return false;
        if (j.is_discarded() || !j.contains("data")) return false;
        const std::string code = json_utils::string_or(j["data"], "code");
        if (code.empty()) {
            fprintf(stderr, "[sigaw] AUTHORIZE rejected: %s\n",
                    j.dump(2).c_str());
            return false;
        }
        fprintf(stderr, "[sigaw] Got auth code, exchanging for token...\n");

        /* Exchange code for access_token via Discord HTTP API */
        token = exchange_token(code, secret);
        if (token.empty()) {
            fprintf(stderr, "[sigaw] Token exchange failed\n");
            return false;
        }

        save_token(token);
        if (!authenticate(token)) {
            return false;
        }

        if (!scope_satisfied(require_messages_read)) {
            fprintf(stderr, "[sigaw] Authenticated token missing required scopes\n");
            clear_auth_state();
            return false;
        }

        return true;
    }

    /* Subscribe to VOICE_CHANNEL_SELECT (global). */
    bool subscribe_voice() {
        if (!authed_) return false;
        return sub("VOICE_CHANNEL_SELECT");
    }

    /* Subscribe to per-channel events. */
    bool subscribe_channel(const std::string& ch_id) {
        json args = {{"channel_id", ch_id}};
        return sub("VOICE_STATE_CREATE", args) &&
               sub("VOICE_STATE_UPDATE", args) &&
               sub("VOICE_STATE_DELETE", args) &&
               sub("SPEAKING_START", args) &&
               sub("SPEAKING_STOP", args);
    }

    bool unsubscribe_channel(const std::string& ch_id) {
        json args = {{"channel_id", ch_id}};
        return unsub("VOICE_STATE_CREATE", args) &&
               unsub("VOICE_STATE_UPDATE", args) &&
               unsub("VOICE_STATE_DELETE", args) &&
               unsub("SPEAKING_START", args) &&
               unsub("SPEAKING_STOP", args);
    }

    bool subscribe_channel_messages(const std::string& ch_id) {
        json args = {{"channel_id", ch_id}};
        return sub("MESSAGE_CREATE", args) &&
               sub("MESSAGE_UPDATE", args) &&
               sub("MESSAGE_DELETE", args);
    }

    bool unsubscribe_channel_messages(const std::string& ch_id) {
        json args = {{"channel_id", ch_id}};
        return unsub("MESSAGE_CREATE", args) &&
               unsub("MESSAGE_UPDATE", args) &&
               unsub("MESSAGE_DELETE", args);
    }

    /* Get current voice channel state (blocking RPC call). */
    VoiceState get_selected_voice_channel() {
        json req = {
            {"cmd", "GET_SELECTED_VOICE_CHANNEL"},
            {"nonce", nonce()},
            {"args", json::object()}
        };
        json j;
        if (!send_request(req, j)) {
            return {};
        }

        VoiceState vs;
        if (j.is_discarded() || !j.contains("data") || j["data"].is_null())
            return vs;

        auto& d = j["data"];
        vs.channel_id   = json_utils::string_or(d, "id");
        vs.channel_name = json_utils::string_or(d, "name");
        vs.guild_id     = json_utils::string_or(d, "guild_id");

        if (d.contains("voice_states")) {
            for (auto& e : d["voice_states"]) {
                VoiceUser u;
                if (const auto* eu = json_utils::object_or_null(e, "user")) {
                    u.id       = json_utils::u64_or(*eu, "id");
                    u.username = json_utils::voice_display_name(e, eu);
                    u.avatar   = json_utils::string_or(*eu, "avatar");
                }
                auto& v = e.contains("voice_state") ? e["voice_state"] : e;
                u.self_mute   = json_utils::bool_or(v, "self_mute", false);
                u.self_deaf   = json_utils::bool_or(v, "self_deaf", false);
                u.server_mute = json_utils::bool_or(v, "mute", false);
                u.server_deaf = json_utils::bool_or(v, "deaf", false);
                vs.users.push_back(u);
            }
        }
        return vs;
    }

    bool get_channel_messages(const std::string& channel_id, size_t limit, json& messages) {
        messages = json::array();
        if (!authed_ || access_token_.empty() || channel_id.empty()) {
            return false;
        }

        const size_t clamped_limit = std::clamp<size_t>(limit, 1u, 100u);
        CURL* curl = curl_easy_init();
        if (!curl) {
            return false;
        }

        std::string response;
        const std::string url =
            "https://discord.com/api/v10/channels/" + channel_id +
            "/messages?limit=" + std::to_string(clamped_limit);
        const std::string auth_header = "Authorization: Bearer " + access_token_;
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, auth_header.c_str());
        headers = curl_slist_append(headers, "Accept: application/json");

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

        const CURLcode res = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            fprintf(stderr, "[sigaw] curl error: %s\n", curl_easy_strerror(res));
            return false;
        }
        if (status < 200 || status >= 300) {
            fprintf(stderr, "[sigaw] Message history request failed: HTTP %ld\n", status);
            return false;
        }

        auto parsed = json::parse(response, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_array()) {
            fprintf(stderr, "[sigaw] Invalid message history response\n");
            return false;
        }

        messages = std::move(parsed);
        return true;
    }

    /* Non-blocking event poll. Returns empty json if nothing pending. */
    json poll_event(int timeout_ms = 0) {
        json queued;
        if (inbox_.take_dispatch(queued)) {
            return queued;
        }

        while (fd_ >= 0) {
            struct pollfd pfd = {fd_, POLLIN, 0};
            if (::poll(&pfd, 1, timeout_ms) <= 0) return {};
            if (!(pfd.revents & POLLIN)) return {};

            Opcode op;
            std::string data;
            if (!recv(op, data)) {
                connected_ = false;
                return {{"_closed", true}};
            }

            if (op == Opcode::Ping) {
                send(Opcode::Pong, data);
                continue;
            }
            if (op == Opcode::Close) {
                connected_ = false;
                return {{"_closed", true}};
            }

            auto j = json::parse(data, nullptr, false);
            if (j.is_discarded()) {
                continue;
            }

            inbox_.store(j);
            if (inbox_.take_dispatch(queued)) {
                return queued;
            }
        }

        return {};
    }

    int fd() const { return fd_; }
    bool is_connected()    const { return connected_; }
    bool is_authenticated() const { return authed_; }
    bool has_scope(std::string_view scope) const {
        return std::find(scopes_.begin(), scopes_.end(), scope) != scopes_.end();
    }
    const DiscordUserIdentity& local_user() const { return inbox_.local_user(); }

private:
    std::string client_id_;
    int fd_ = -1;
    bool connected_ = false, authed_ = false;
    uint64_t nonce_ctr_ = 0;
    DiscordMessageInbox inbox_;
    std::string access_token_;
    std::vector<std::string> scopes_;

    void clear_auth_state() {
        authed_ = false;
        access_token_.clear();
        scopes_.clear();
    }

    bool scope_satisfied(bool require_messages_read) const {
        return !require_messages_read || has_scope("messages.read");
    }

    bool try_sock(const std::string& path) {
        int s = socket(AF_UNIX, SOCK_STREAM, 0);
        if (s < 0) return false;
        sockaddr_un addr = {};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path)-1);
        if (::connect(s, (sockaddr*)&addr, sizeof(addr)) == 0) {
            fd_ = s; connected_ = true;
            fprintf(stderr, "[sigaw] Connected: %s\n", path.c_str());
            return true;
        }
        ::close(s);
        return false;
    }

    bool send(Opcode op, const std::string& payload) {
        if (fd_ < 0) return false;
        uint32_t o = (uint32_t)op, l = (uint32_t)payload.size();
        std::vector<uint8_t> buf(8 + l);
        memcpy(&buf[0], &o, 4);
        memcpy(&buf[4], &l, 4);
        memcpy(&buf[8], payload.data(), l);
        return write_all(buf.data(), buf.size()) == (ssize_t)buf.size();
    }

    bool recv(Opcode& op, std::string& payload) {
        if (fd_ < 0) return false;
        uint8_t hdr[8];
        if (read_all(hdr, 8) != 8) return false;
        uint32_t o, l;
        memcpy(&o, hdr, 4); memcpy(&l, hdr+4, 4);
        op = (Opcode)o;
        if (l > 4*1024*1024) return false; /* 4MB sanity */
        payload.resize(l);
        return l == 0 || read_all((uint8_t*)payload.data(), l) == (ssize_t)l;
    }

    ssize_t read_all(uint8_t* buf, size_t n) {
        size_t got = 0;
        while (got < n) {
            ssize_t r = ::read(fd_, buf+got, n-got);
            if (r <= 0) return r;
            got += r;
        }
        return got;
    }

    ssize_t write_all(const uint8_t* buf, size_t n) {
        size_t sent = 0;
        while (sent < n) {
            ssize_t w = ::write(fd_, buf + sent, n - sent);
            if (w <= 0) return w;
            sent += w;
        }
        return sent;
    }

    std::string nonce() { return "s-" + std::to_string(++nonce_ctr_); }

    bool send_request(const json& request, json& response) {
        const std::string nonce_value = json_utils::string_or(request, "nonce");
        const std::string cmd_value = json_utils::string_or(request, "cmd");
        if (!send(Opcode::Frame, request.dump())) {
            return false;
        }
        return wait_for_response(nonce_value, cmd_value, response);
    }

    bool wait_for_response(const std::string& nonce_value,
                           const std::string& expected_cmd,
                           json& response)
    {
        if (inbox_.take_response(nonce_value, expected_cmd, response)) {
            return true;
        }

        while (fd_ >= 0) {
            Opcode op;
            std::string payload;
            if (!recv(op, payload)) {
                connected_ = false;
                return false;
            }

            if (op == Opcode::Ping) {
                send(Opcode::Pong, payload);
                continue;
            }
            if (op == Opcode::Close) {
                connected_ = false;
                return false;
            }

            auto j = json::parse(payload, nullptr, false);
            if (j.is_discarded()) {
                continue;
            }

            if (inbox_.store_or_match(j, nonce_value, expected_cmd, response)) {
                return true;
            }
        }

        return false;
    }

    bool sub(const std::string& evt, json args = json::object()) {
        json request = {
            {"cmd", "SUBSCRIBE"},
            {"nonce", nonce()},
            {"evt", evt},
            {"args", args},
        };
        json response;
        if (!send_request(request, response)) {
            return false;
        }
        return json_utils::string_or(response, "evt") != "ERROR";
    }

    bool unsub(const std::string& evt, json args = json::object()) {
        json request = {
            {"cmd", "UNSUBSCRIBE"},
            {"nonce", nonce()},
            {"evt", evt},
            {"args", args},
        };
        json response;
        if (!send_request(request, response)) {
            return false;
        }
        return json_utils::string_or(response, "evt") != "ERROR";
    }

    bool authenticate(const std::string& token) {
        clear_auth_state();
        json req = {
            {"cmd", "AUTHENTICATE"},
            {"nonce", nonce()},
            {"args", {{"access_token", token}}}
        };
        json j;
        if (!send_request(req, j)) return false;
        if (j.is_discarded()) return false;
        if (json_utils::string_or(j, "cmd") == "AUTHENTICATE" && j.contains("data")) {
            if (const auto* data = json_utils::object_or_null(j, "data")) {
                const auto it = data->find("scopes");
                if (it != data->end() && it->is_array()) {
                    for (const auto& scope : *it) {
                        const auto scope_name = json_utils::string_value(scope);
                        if (!scope_name.empty()) {
                            scopes_.push_back(scope_name);
                        }
                    }
                }
            }
            access_token_ = token;
            authed_ = true;
            fprintf(stderr, "[sigaw] Authenticated\n");
            return true;
        }
        fprintf(stderr, "[sigaw] AUTHENTICATE failed: %s\n",
                json_utils::string_or(j, "evt").c_str());
        return false;
    }

    /* ---- Token exchange via libcurl ---- */

    static size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* ud) {
        auto* s = (std::string*)ud;
        s->append(ptr, size * nmemb);
        return size * nmemb;
    }

    std::string exchange_token(const std::string& code,
                                const std::string& secret)
    {
        CURL* curl = curl_easy_init();
        if (!curl) return "";

        char* esc_code = curl_easy_escape(curl, code.c_str(), 0);
        char* esc_client = curl_easy_escape(curl, client_id_.c_str(), 0);
        char* esc_secret = curl_easy_escape(curl, secret.c_str(), 0);
        if (!esc_code || !esc_client || !esc_secret) {
            if (esc_code) curl_free(esc_code);
            if (esc_client) curl_free(esc_client);
            if (esc_secret) curl_free(esc_secret);
            curl_easy_cleanup(curl);
            return "";
        }

        std::string post =
            "grant_type=authorization_code"
            "&code=" + std::string(esc_code) +
            "&client_id=" + std::string(esc_client) +
            "&client_secret=" + std::string(esc_secret) +
            "&redirect_uri=http%3A%2F%2Flocalhost%3A29847%2Fcallback";

        curl_free(esc_code);
        curl_free(esc_client);
        curl_free(esc_secret);

        std::string response;
        curl_easy_setopt(curl, CURLOPT_URL,
                         "https://discord.com/api/oauth2/token");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, post.size());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            fprintf(stderr, "[sigaw] curl error: %s\n",
                    curl_easy_strerror(res));
            return "";
        }

        auto j = json::parse(response, nullptr, false);
        if (j.is_discarded() || !j.contains("access_token")) {
            fprintf(stderr, "[sigaw] Token response: %s\n", response.c_str());
            return "";
        }

        return j["access_token"];
    }

    /* ---- Token persistence ---- */

    static std::filesystem::path token_path() {
        const char* xdg = getenv("XDG_CONFIG_HOME");
        std::filesystem::path base;
        if (xdg && *xdg) base = xdg;
        else {
            const char* h = getenv("HOME");
            base = h ? std::filesystem::path(h) / ".config" : "/tmp";
        }
        return base / "sigaw" / ".token";
    }

    static std::string load_token() {
        auto p = token_path();
        if (!std::filesystem::exists(p)) return "";
        std::ifstream f(p);
        std::string tok;
        std::getline(f, tok);
        return tok;
    }

    static void save_token(const std::string& token) {
        auto p = token_path();
        std::filesystem::create_directories(p.parent_path());
        std::ofstream f(p);
        f << token;
        /* Set restrictive permissions */
        std::filesystem::permissions(p,
            std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write);
    }
};

} /* namespace sigaw */

#endif /* SIGAW_DISCORD_IPC_H */
