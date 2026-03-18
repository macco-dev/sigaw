#ifndef SIGAW_CONFIG_H
#define SIGAW_CONFIG_H

#include <algorithm>
#include <string>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <unordered_map>
#include <cstdlib>
#include <system_error>
#include <unistd.h>

#include "protocol.h"

namespace sigaw {

enum class OverlayPosition {
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
};

struct Config {
    /* Overlay appearance */
    OverlayPosition position     = OverlayPosition::TopRight;
    float           scale        = 1.0f;
    float           opacity      = 0.72f;
    bool            show_avatars = true;
    bool            show_channel = false;
    bool            show_voice_channel_chat = false;
    bool            compact      = false;
    int             max_visible  = 8;
    int             max_visible_chat_messages = 4;

    /* Daemon settings */
    std::string client_id        = "1483519089719640105";  /* Discord application ID */
    std::string client_secret    = "HD2VNfR0HxOiLSMM842PMDDv-SbW1r6Z";  /* Discord application secret */

    /* Internal */
    bool        visible          = true;

    inline static std::filesystem::path override_path_{};
    inline static bool                  has_override_path_ = false;

    static void set_override_path(const std::filesystem::path& path) {
        override_path_ = path;
        has_override_path_ = true;
    }

    static void clear_override_path() {
        override_path_.clear();
        has_override_path_ = false;
    }

    static Config load() {
        Config cfg;
        auto path = config_path();

        if (!std::filesystem::exists(path)) {
            return cfg;
        }

        std::ifstream file(path);
        if (!file.is_open()) return cfg;

        std::string line;
        std::unordered_map<std::string, std::string> kv;

        while (std::getline(file, line)) {
            /* Skip comments and empty lines */
            if (line.empty() || line[0] == '#') continue;

            auto eq = line.find('=');
            if (eq == std::string::npos) continue;

            auto key = line.substr(0, eq);
            auto val = line.substr(eq + 1);

            /* Trim whitespace */
            auto trim = [](std::string& s) {
                const auto first = s.find_first_not_of(" \t");
                if (first == std::string::npos) {
                    s.clear();
                    return;
                }
                const auto last = s.find_last_not_of(" \t");
                s = s.substr(first, last - first + 1);
            };
            trim(key);
            trim(val);

            kv[key] = val;
        }

        auto get_str = [&](const char* k) -> std::string {
            auto it = kv.find(k);
            return (it != kv.end()) ? it->second : "";
        };

        auto get_float = [&](const char* k, float def) -> float {
            auto s = get_str(k);
            if (s.empty()) return def;
            try {
                return std::stof(s);
            } catch (...) {
                return def;
            }
        };

        auto get_int = [&](const char* k, int def) -> int {
            auto s = get_str(k);
            if (s.empty()) return def;
            try {
                return std::stoi(s);
            } catch (...) {
                return def;
            }
        };

        auto get_bool = [&](const char* k, bool def) -> bool {
            auto s = get_str(k);
            if (s.empty()) return def;
            return (s == "true" || s == "1" || s == "yes");
        };

        /* Parse position */
        auto pos_str = get_str("position");
        if (pos_str == "top-left")     cfg.position = OverlayPosition::TopLeft;
        if (pos_str == "top-right")    cfg.position = OverlayPosition::TopRight;
        if (pos_str == "bottom-left")  cfg.position = OverlayPosition::BottomLeft;
        if (pos_str == "bottom-right") cfg.position = OverlayPosition::BottomRight;

        cfg.scale        = get_float("scale", cfg.scale);
        cfg.opacity      = get_float("opacity", cfg.opacity);
        cfg.show_avatars = get_bool("show_avatars", cfg.show_avatars);
        cfg.show_channel = get_bool("show_channel_name", cfg.show_channel);
        cfg.show_voice_channel_chat =
            get_bool("show_voice_channel_chat", cfg.show_voice_channel_chat);
        cfg.compact      = get_bool("compact", cfg.compact);
        cfg.max_visible  = get_int("max_visible_users", cfg.max_visible);
        cfg.max_visible_chat_messages = std::clamp(
            get_int("max_visible_chat_messages", cfg.max_visible_chat_messages),
            1,
            SIGAW_MAX_CHAT_MESSAGES
        );
        cfg.visible      = get_bool("visible", cfg.visible);

        if (const auto client_id = get_str("client_id"); !client_id.empty()) {
            cfg.client_id = client_id;
        }
        if (const auto client_secret = get_str("client_secret"); !client_secret.empty()) {
            cfg.client_secret = client_secret;
        }

        return cfg;
    }

    static std::filesystem::path config_path() {
        if (has_override_path_) {
            return override_path_;
        }

        const char* env_override = std::getenv("SIGAW_CONFIG");
        if (env_override && *env_override) {
            return env_override;
        }

        const char* xdg = std::getenv("XDG_CONFIG_HOME");
        std::filesystem::path base;
        if (xdg && *xdg) {
            base = xdg;
        } else {
            const char* home = std::getenv("HOME");
            base = home ? std::filesystem::path(home) / ".config" : "/tmp";
        }
        return base / "sigaw" / "sigaw.conf";
    }

    static std::filesystem::path runtime_dir() {
        const char* xdg = std::getenv("XDG_RUNTIME_DIR");
        if (xdg && *xdg) {
            return xdg;
        }

        const auto uid = std::to_string(static_cast<unsigned long>(::getuid()));
        const auto run_user = std::filesystem::path("/run/user") / uid;
        std::error_code ec;
        if (std::filesystem::exists(run_user, ec)) {
            return run_user;
        }

        auto fallback = std::filesystem::temp_directory_path(ec);
        if (ec || fallback.empty()) {
            fallback = "/tmp";
        }

        fallback /= "sigaw-" + uid;
        std::filesystem::create_directories(fallback, ec);
        std::filesystem::permissions(
            fallback,
            std::filesystem::perms::owner_all,
            std::filesystem::perm_options::replace,
            ec
        );
        return fallback;
    }

    static std::filesystem::path cache_dir() {
        const char* xdg = std::getenv("XDG_CACHE_HOME");
        std::filesystem::path base;
        if (xdg && *xdg) {
            base = xdg;
        } else {
            const char* home = std::getenv("HOME");
            base = home ? std::filesystem::path(home) / ".cache" : runtime_dir() / "cache";
        }

        std::error_code ec;
        std::filesystem::create_directories(base / "sigaw", ec);
        return base / "sigaw";
    }

    static std::filesystem::path avatar_cache_dir() {
        auto dir = cache_dir() / "avatars";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir;
    }

    static std::filesystem::path avatar_cache_path(uint64_t user_id, std::string_view avatar_hash) {
        for (char c : avatar_hash) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
                return {};
            }
        }
        return avatar_cache_dir() /
               (std::to_string(static_cast<unsigned long long>(user_id)) + "-" +
                std::string(avatar_hash) + ".png");
    }

    static std::filesystem::path control_socket_path() {
        return runtime_dir() / "sigaw-ctl.sock";
    }

    static std::string shared_memory_name() {
        const char* env_override = std::getenv("SIGAW_SHM_NAME");
        if (env_override && *env_override) {
            return env_override;
        }

        return "/sigaw-voice-" +
               std::to_string(static_cast<unsigned long long>(::getuid()));
    }

    static std::string overlay_frame_memory_name() {
        const char* env_override = std::getenv("SIGAW_FRAME_SHM_NAME");
        if (env_override && *env_override) {
            return env_override;
        }

        return "/sigaw-overlay-" +
               std::to_string(static_cast<unsigned long long>(::getuid()));
    }

    bool save() const {
        return write_to_path(config_path());
    }

    void write_default() const {
        (void)write_to_path(config_path());
    }

private:
    static const char* position_name(OverlayPosition position) {
        switch (position) {
            case OverlayPosition::TopLeft:     return "top-left";
            case OverlayPosition::TopRight:    return "top-right";
            case OverlayPosition::BottomLeft:  return "bottom-left";
            case OverlayPosition::BottomRight: return "bottom-right";
        }
        return "top-right";
    }

    bool write_to_path(const std::filesystem::path& path) const {
        std::filesystem::create_directories(path.parent_path());

        std::ofstream f(path);
        if (!f.is_open()) {
            return false;
        }

        f << "# Sigaw configuration\n"
          << "# Discord voice overlay for Linux (Vulkan + OpenGL)\n"
          << "#\n"
          << "# Position: top-left, top-right, bottom-left, bottom-right\n"
          << "position=" << position_name(position) << "\n"
          << "\n"
          << "# Scale multiplier (1.0 = default)\n"
          << "scale=" << scale << "\n"
          << "\n"
          << "# Overlay opacity (0.0 = transparent, 1.0 = opaque)\n"
          << "opacity=" << opacity << "\n"
          << "\n"
          << "# Show user avatars\n"
          << "show_avatars=" << (show_avatars ? "true" : "false") << "\n"
          << "\n"
          << "# Show channel name at the top\n"
          << "show_channel_name=" << (show_channel ? "true" : "false") << "\n"
          << "\n"
          << "# Show the latest voice channel chat messages under the user list\n"
          << "show_voice_channel_chat=" << (show_voice_channel_chat ? "true" : "false") << "\n"
          << "\n"
          << "# Compact mode (small icons, no usernames)\n"
          << "compact=" << (compact ? "true" : "false") << "\n"
          << "\n"
          << "# Maximum users to display\n"
          << "max_visible_users=" << max_visible << "\n"
          << "\n"
          << "# Maximum voice channel chat messages to display\n"
          << "max_visible_chat_messages=" << max_visible_chat_messages << "\n"
          << "\n"
          << "# Persisted overlay visibility (managed by sigaw-ctl)\n"
          << "visible=" << (visible ? "true" : "false") << "\n"
          << "\n"
          << "# Discord application credentials\n"
          << "# Sigaw ships with public default credentials.\n"
          << "# Replace these if you want to use your own Discord application.\n"
          << "client_id=" << client_id << "\n"
          << "client_secret=" << client_secret << "\n";
        return f.good();
    }

};

} /* namespace sigaw */

#endif /* SIGAW_CONFIG_H */
