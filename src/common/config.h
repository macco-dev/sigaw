#ifndef SIGAW_CONFIG_H
#define SIGAW_CONFIG_H

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unistd.h>
#include <vector>

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
        return load_for_executable(std::string_view{});
    }

    static Config load_for_executable(std::string_view executable_basename) {
        const ParsedConfigFile parsed = parse_file(config_path());
        Config cfg;
        apply_entries(cfg, parsed.global_entries, true);

        if (!executable_basename.empty()) {
            const auto it = parsed.profile_entries.find(std::string(executable_basename));
            if (it != parsed.profile_entries.end()) {
                apply_entries(cfg, it->second, false);
            }
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

    static std::string current_executable_basename() {
        std::error_code ec;
        const auto target = std::filesystem::read_symlink("/proc/self/exe", ec);
        if (ec) {
            return {};
        }
        return target.filename().string();
    }

    bool save() const {
        return write_to_path(config_path());
    }

    void write_default() const {
        (void)write_to_path(config_path());
    }

private:
    struct ParsedConfigFile {
        std::unordered_map<std::string, std::string> global_entries;
        std::unordered_map<std::string, std::unordered_map<std::string, std::string>> profile_entries;
    };

    enum class SectionKind {
        Global,
        Profile,
        Other,
    };

    struct Section {
        SectionKind kind = SectionKind::Global;
        std::string name;
    };

    static std::string trim_copy(std::string_view value) {
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos) {
            return {};
        }
        const auto last = value.find_last_not_of(" \t\r\n");
        return std::string(value.substr(first, last - first + 1));
    }

    static bool parse_key_value(std::string_view line, std::string& key, std::string& value) {
        const auto eq = line.find('=');
        if (eq == std::string_view::npos) {
            return false;
        }

        key = trim_copy(line.substr(0, eq));
        value = trim_copy(line.substr(eq + 1));
        return !key.empty();
    }

    static bool parse_section_header(std::string_view line, Section& section) {
        const std::string trimmed = trim_copy(line);
        if (trimmed.size() < 2 || trimmed.front() != '[' || trimmed.back() != ']') {
            return false;
        }

        const std::string name = trim_copy(
            std::string_view(trimmed).substr(1, trimmed.size() - 2)
        );
        if (name.rfind("profile:", 0) == 0) {
            section.kind = SectionKind::Profile;
            section.name = trim_copy(std::string_view(name).substr(8));
            if (section.name.empty()) {
                section.kind = SectionKind::Other;
            }
            return true;
        }

        section.kind = SectionKind::Other;
        section.name = name;
        return true;
    }

    static bool parse_bool(std::string_view value, bool def) {
        const std::string lowered = trim_copy(value);
        if (lowered.empty()) {
            return def;
        }
        return lowered == "true" || lowered == "1" || lowered == "yes";
    }

    static float parse_float(std::string_view value, float def) {
        const std::string trimmed = trim_copy(value);
        if (trimmed.empty()) {
            return def;
        }
        try {
            return std::stof(trimmed);
        } catch (...) {
            return def;
        }
    }

    static int parse_int(std::string_view value, int def) {
        const std::string trimmed = trim_copy(value);
        if (trimmed.empty()) {
            return def;
        }
        try {
            return std::stoi(trimmed);
        } catch (...) {
            return def;
        }
    }

    static bool is_overlay_key(std::string_view key) {
        return key == "position" ||
               key == "scale" ||
               key == "opacity" ||
               key == "show_avatars" ||
               key == "show_channel_name" ||
               key == "show_voice_channel_chat" ||
               key == "compact" ||
               key == "max_visible_users" ||
               key == "max_visible_chat_messages" ||
               key == "visible";
    }

    static void apply_entry(Config& cfg, std::string_view key, std::string_view value,
                            bool allow_daemon_keys)
    {
        if (key == "position") {
            if (value == "top-left")     cfg.position = OverlayPosition::TopLeft;
            if (value == "top-right")    cfg.position = OverlayPosition::TopRight;
            if (value == "bottom-left")  cfg.position = OverlayPosition::BottomLeft;
            if (value == "bottom-right") cfg.position = OverlayPosition::BottomRight;
            return;
        }

        if (key == "scale") {
            cfg.scale = parse_float(value, cfg.scale);
            return;
        }
        if (key == "opacity") {
            cfg.opacity = parse_float(value, cfg.opacity);
            return;
        }
        if (key == "show_avatars") {
            cfg.show_avatars = parse_bool(value, cfg.show_avatars);
            return;
        }
        if (key == "show_channel_name") {
            cfg.show_channel = parse_bool(value, cfg.show_channel);
            return;
        }
        if (key == "show_voice_channel_chat") {
            cfg.show_voice_channel_chat = parse_bool(value, cfg.show_voice_channel_chat);
            return;
        }
        if (key == "compact") {
            cfg.compact = parse_bool(value, cfg.compact);
            return;
        }
        if (key == "max_visible_users") {
            cfg.max_visible = parse_int(value, cfg.max_visible);
            return;
        }
        if (key == "max_visible_chat_messages") {
            cfg.max_visible_chat_messages = std::clamp(
                parse_int(value, cfg.max_visible_chat_messages),
                1,
                SIGAW_MAX_CHAT_MESSAGES
            );
            return;
        }
        if (key == "visible") {
            cfg.visible = parse_bool(value, cfg.visible);
            return;
        }

        if (!allow_daemon_keys) {
            return;
        }

        if (key == "client_id") {
            const std::string trimmed = trim_copy(value);
            if (!trimmed.empty()) {
                cfg.client_id = trimmed;
            }
            return;
        }
        if (key == "client_secret") {
            const std::string trimmed = trim_copy(value);
            if (!trimmed.empty()) {
                cfg.client_secret = trimmed;
            }
        }
    }

    static void apply_entries(Config& cfg,
                              const std::unordered_map<std::string, std::string>& entries,
                              bool allow_daemon_keys)
    {
        for (const auto& [key, value] : entries) {
            apply_entry(cfg, key, value, allow_daemon_keys);
        }
    }

    static ParsedConfigFile parse_file(const std::filesystem::path& path) {
        ParsedConfigFile parsed;
        std::ifstream file(path);
        if (!file.is_open()) {
            return parsed;
        }

        Section section;
        std::string line;
        while (std::getline(file, line)) {
            const std::string trimmed = trim_copy(line);
            if (trimmed.empty() || trimmed[0] == '#') {
                continue;
            }

            Section next_section;
            if (parse_section_header(trimmed, next_section)) {
                section = next_section;
                continue;
            }

            std::string key;
            std::string value;
            if (!parse_key_value(trimmed, key, value)) {
                continue;
            }

            if (section.kind == SectionKind::Global) {
                parsed.global_entries[key] = value;
                continue;
            }

            if (section.kind == SectionKind::Profile && is_overlay_key(key)) {
                parsed.profile_entries[section.name][key] = value;
            }
        }

        return parsed;
    }

    static const char* position_name(OverlayPosition position) {
        switch (position) {
            case OverlayPosition::TopLeft:     return "top-left";
            case OverlayPosition::TopRight:    return "top-right";
            case OverlayPosition::BottomLeft:  return "bottom-left";
            case OverlayPosition::BottomRight: return "bottom-right";
        }
        return "top-right";
    }

    static const std::array<const char*, 12>& known_global_keys() {
        static const std::array<const char*, 12> keys = {
            "position",
            "scale",
            "opacity",
            "show_avatars",
            "show_channel_name",
            "show_voice_channel_chat",
            "compact",
            "max_visible_users",
            "max_visible_chat_messages",
            "visible",
            "client_id",
            "client_secret",
        };
        return keys;
    }

    static bool is_known_global_key(std::string_view key) {
        for (const char* known_key : known_global_keys()) {
            if (key == known_key) {
                return true;
            }
        }
        return false;
    }

    static std::string bool_name(bool value) {
        return value ? "true" : "false";
    }

    std::vector<std::string> serialized_global_entries() const {
        return {
            std::string("position=") + position_name(position),
            "scale=" + std::to_string(scale),
            "opacity=" + std::to_string(opacity),
            "show_avatars=" + bool_name(show_avatars),
            "show_channel_name=" + bool_name(show_channel),
            "show_voice_channel_chat=" + bool_name(show_voice_channel_chat),
            "compact=" + bool_name(compact),
            "max_visible_users=" + std::to_string(max_visible),
            "max_visible_chat_messages=" + std::to_string(max_visible_chat_messages),
            "visible=" + bool_name(visible),
            "client_id=" + client_id,
            "client_secret=" + client_secret,
        };
    }

    std::string default_file_contents() const {
        std::string out;
        auto append = [&](std::string_view line) {
            out.append(line);
            out.push_back('\n');
        };

        append("# Sigaw configuration");
        append("# Discord voice overlay for Linux (Vulkan + OpenGL)");
        append("#");
        append("# Position: top-left, top-right, bottom-left, bottom-right");
        append(std::string("position=") + position_name(position));
        append("");
        append("# Scale multiplier (1.0 = default)");
        append("scale=" + std::to_string(scale));
        append("");
        append("# Overlay opacity (0.0 = transparent, 1.0 = opaque)");
        append("opacity=" + std::to_string(opacity));
        append("");
        append("# Show user avatars");
        append("show_avatars=" + bool_name(show_avatars));
        append("");
        append("# Show channel name at the top");
        append("show_channel_name=" + bool_name(show_channel));
        append("");
        append("# Show the latest voice channel chat messages under the user list");
        append("show_voice_channel_chat=" + bool_name(show_voice_channel_chat));
        append("");
        append("# Compact mode (small icons, no usernames)");
        append("compact=" + bool_name(compact));
        append("");
        append("# Maximum users to display");
        append("max_visible_users=" + std::to_string(max_visible));
        append("");
        append("# Maximum voice channel chat messages to display");
        append("max_visible_chat_messages=" + std::to_string(max_visible_chat_messages));
        append("");
        append("# Persisted overlay visibility (managed by sigaw-ctl)");
        append("visible=" + bool_name(visible));
        append("");
        append("# Discord application credentials");
        append("# Sigaw ships with public default credentials.");
        append("# Replace these if you want to use your own Discord application.");
        append("client_id=" + client_id);
        append("client_secret=" + client_secret);
        append("");
        append("# Optional exact executable-basename profiles.");
        append("# Matching is case-sensitive and uses /proc/self/exe, e.g. [profile:vkcube].");
        append("#");
        append("# [profile:vkcube]");
        append("# position=bottom-left");
        append("# compact=true");
        append("# show_voice_channel_chat=true");
        return out;
    }

    bool update_existing_file(const std::filesystem::path& path) const {
        std::ifstream input(path);
        if (!input.is_open()) {
            return false;
        }

        std::vector<std::string> lines;
        std::string line;
        while (std::getline(input, line)) {
            lines.push_back(line);
        }

        std::unordered_map<std::string, std::vector<size_t>> global_key_lines;
        size_t first_section_line = lines.size();
        Section section;

        for (size_t i = 0; i < lines.size(); ++i) {
            const std::string trimmed = trim_copy(lines[i]);
            if (trimmed.empty() || trimmed[0] == '#') {
                continue;
            }

            Section next_section;
            if (parse_section_header(trimmed, next_section)) {
                if (first_section_line == lines.size()) {
                    first_section_line = i;
                }
                section = next_section;
                continue;
            }

            if (section.kind != SectionKind::Global) {
                continue;
            }

            std::string key;
            std::string value;
            if (!parse_key_value(trimmed, key, value) || !is_known_global_key(key)) {
                continue;
            }
            global_key_lines[key].push_back(i);
        }

        const auto entries = serialized_global_entries();
        const auto& known_keys = known_global_keys();
        std::vector<std::string> missing_entries;
        for (size_t i = 0; i < known_keys.size(); ++i) {
            const std::string formatted = entries[i];
            const std::string key = std::string(known_keys[i]);
            const auto found = global_key_lines.find(key);
            if (found == global_key_lines.end()) {
                missing_entries.push_back(formatted);
                continue;
            }

            for (const size_t line_index : found->second) {
                lines[line_index] = formatted;
            }
        }

        if (!missing_entries.empty()) {
            const size_t insert_at = first_section_line;
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insert_at),
                         missing_entries.begin(), missing_entries.end());
        }

        std::ofstream output(path);
        if (!output.is_open()) {
            return false;
        }

        for (size_t i = 0; i < lines.size(); ++i) {
            output << lines[i] << '\n';
        }
        return output.good();
    }

    bool write_to_path(const std::filesystem::path& path) const {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);

        if (std::filesystem::exists(path, ec)) {
            return update_existing_file(path);
        }

        std::ofstream f(path);
        if (!f.is_open()) {
            return false;
        }

        f << default_file_contents();
        return f.good();
    }

};

} /* namespace sigaw */

#endif /* SIGAW_CONFIG_H */
