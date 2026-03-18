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

} // namespace

int main() {
    if (!test_chat_config_round_trip()) {
        return 1;
    }
    if (!test_chat_message_count_is_clamped()) {
        return 1;
    }
    return 0;
}
