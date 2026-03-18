#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

#include "common/config.h"

namespace {

struct ConfigEnvGuard {
    explicit ConfigEnvGuard(const std::filesystem::path& path)
        : path_(path), dir_(path.parent_path())
    {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
        std::filesystem::create_directories(dir_, ec);
        setenv("SIGAW_CONFIG", path_.c_str(), 1);
    }

    ~ConfigEnvGuard() {
        unsetenv("SIGAW_CONFIG");
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    std::filesystem::path path_;
    std::filesystem::path dir_;
};

std::filesystem::path unique_config_path() {
    const auto suffix = std::to_string(static_cast<unsigned long long>(::getpid()));
    return std::filesystem::temp_directory_path() / ("sigaw-config-parse-" + suffix) / "sigaw.conf";
}

void write_file(const std::filesystem::path& path, const std::string& contents) {
    std::ofstream out(path);
    out << contents;
}

bool test_chat_config_round_trip() {
    ConfigEnvGuard guard(unique_config_path());

    sigaw::Config cfg;
    cfg.show_voice_channel_chat = true;
    cfg.max_visible_chat_messages = 6;
    if (!cfg.save()) {
        std::cerr << "failed to save chat config\n";
        return false;
    }

    const auto loaded = sigaw::Config::load();
    if (!loaded.show_voice_channel_chat || loaded.max_visible_chat_messages != 6) {
        std::cerr << "chat config values should round-trip through Config::save/load\n";
        return false;
    }

    return true;
}

bool test_chat_message_count_is_clamped() {
    ConfigEnvGuard guard(unique_config_path());
    write_file(
        guard.path_,
        "show_voice_channel_chat=true\n"
        "max_visible_chat_messages=99\n"
    );

    const auto loaded = sigaw::Config::load();
    if (loaded.max_visible_chat_messages != SIGAW_MAX_CHAT_MESSAGES) {
        std::cerr << "chat message count should clamp to the protocol max\n";
        return false;
    }

    write_file(
        guard.path_,
        "show_voice_channel_chat=true\n"
        "max_visible_chat_messages=0\n"
    );
    const auto clamped_min = sigaw::Config::load();
    if (clamped_min.max_visible_chat_messages != 1) {
        std::cerr << "chat message count should clamp to at least one visible message\n";
        return false;
    }

    return true;
}

bool test_profile_overrides_apply_per_executable() {
    ConfigEnvGuard guard(unique_config_path());
    write_file(
        guard.path_,
        "position=top-right\n"
        "compact=false\n"
        "show_voice_channel_chat=false\n"
        "client_id=global-client\n"
        "\n"
        "[profile:vkcube]\n"
        "position=bottom-left\n"
        "compact=true\n"
        "show_voice_channel_chat=true\n"
        "client_id=ignored-in-profile\n"
        "\n"
        "[profile:other-game]\n"
        "position=top-left\n"
    );

    const auto global = sigaw::Config::load();
    if (global.position != sigaw::OverlayPosition::TopRight ||
        global.compact ||
        global.show_voice_channel_chat ||
        global.client_id != "global-client") {
        std::cerr << "global config should ignore profile-only overrides\n";
        return false;
    }

    const auto vkcube = sigaw::Config::load_for_executable("vkcube");
    if (vkcube.position != sigaw::OverlayPosition::BottomLeft ||
        !vkcube.compact ||
        !vkcube.show_voice_channel_chat ||
        vkcube.client_id != "global-client") {
        std::cerr << "matching executable should inherit globals and apply overlay profile keys\n";
        return false;
    }

    const auto missing = sigaw::Config::load_for_executable("missing-game");
    if (missing.position != sigaw::OverlayPosition::TopRight ||
        missing.compact ||
        missing.show_voice_channel_chat) {
        std::cerr << "non-matching executable should fall back to global settings\n";
        return false;
    }

    return true;
}

bool test_profile_chat_requests_are_detected_for_daemon() {
    ConfigEnvGuard guard(unique_config_path());
    write_file(
        guard.path_,
        "show_voice_channel_chat=false\n"
        "max_visible_chat_messages=4\n"
        "\n"
        "[profile:vkcube]\n"
        "show_voice_channel_chat=true\n"
    );

    if (!sigaw::Config::any_profile_requests_chat()) {
        std::cerr << "profile-only chat enable should be visible to daemon auth\n";
        return false;
    }

    write_file(
        guard.path_,
        "show_voice_channel_chat=false\n"
        "\n"
        "[profile:vkcube]\n"
        "compact=true\n"
    );

    if (sigaw::Config::any_profile_requests_chat()) {
        std::cerr << "non-chat profile overrides should not request daemon chat scope\n";
        return false;
    }

    return true;
}

bool test_save_preserves_profile_sections_and_comments() {
    ConfigEnvGuard guard(unique_config_path());
    write_file(
        guard.path_,
        "# custom comment\n"
        "position=top-right\n"
        "\n"
        "[profile:vkcube]\n"
        "compact=true\n"
        "# keep me\n"
    );

    auto cfg = sigaw::Config::load();
    cfg.visible = false;
    if (!cfg.save()) {
        std::cerr << "failed to save config with profile sections\n";
        return false;
    }

    std::ifstream in(guard.path_);
    const std::string saved((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
    if (saved.find("# custom comment") == std::string::npos ||
        saved.find("[profile:vkcube]") == std::string::npos ||
        saved.find("compact=true") == std::string::npos ||
        saved.find("# keep me") == std::string::npos) {
        std::cerr << "saving globals should preserve existing comments and profile sections\n";
        return false;
    }

    const auto visible_pos = saved.find("visible=false");
    const auto profile_pos = saved.find("[profile:vkcube]");
    if (visible_pos == std::string::npos || profile_pos == std::string::npos ||
        visible_pos > profile_pos) {
        std::cerr << "new global keys should be inserted before the first profile section\n";
        return false;
    }

    return true;
}

} // namespace

int main() {
    if (!test_chat_config_round_trip()) {
        return 1;
    }
    if (!test_chat_message_count_is_clamped()) {
        return 1;
    }
    if (!test_profile_overrides_apply_per_executable()) {
        return 1;
    }
    if (!test_profile_chat_requests_are_detected_for_daemon()) {
        return 1;
    }
    if (!test_save_preserves_profile_sections_and_comments()) {
        return 1;
    }
    return 0;
}
