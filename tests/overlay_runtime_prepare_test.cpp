#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

#include "daemon/shm_writer.h"
#include "layer/overlay_runtime.h"

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

struct ShmEnvGuard {
    explicit ShmEnvGuard(std::string value) : value_(std::move(value)) {
        setenv("SIGAW_SHM_NAME", value_.c_str(), 1);
        shm_unlink(value_.c_str());
    }

    ~ShmEnvGuard() {
        shm_unlink(value_.c_str());
        unsetenv("SIGAW_SHM_NAME");
    }

    std::string value_;
};

std::filesystem::path unique_config_path() {
    const auto suffix = std::to_string(static_cast<unsigned long long>(::getpid()));
    return std::filesystem::temp_directory_path() /
           ("sigaw-overlay-runtime-" + suffix) / "sigaw.conf";
}

std::string unique_shm_name() {
    return "/sigaw-overlay-runtime-" +
           std::to_string(static_cast<unsigned long long>(::getpid()));
}

void write_file(const std::filesystem::path& path, const std::string& contents) {
    std::ofstream out(path);
    out << contents;
}

void write_voice(sigaw::ShmWriter& writer) {
    writer.begin_write();
    writer.set_channel("Lobby");
    writer.set_user_count(1);
    writer.set_user(0, 7, "Alpha", "", false, false, false, false, false);
    writer.set_chat_count(0);
    writer.end_write();
}

bool test_prepare_marks_unchanged_frames_clean() {
    ConfigEnvGuard config_guard(unique_config_path());
    ShmEnvGuard shm_guard(unique_shm_name());
    write_file(config_guard.path_, "show_avatars=false\n");

    sigaw::ShmWriter writer;
    if (!writer.open()) {
        std::cerr << "failed to open overlay runtime SHM writer\n";
        return false;
    }
    write_voice(writer);

    sigaw::overlay::Runtime runtime;
    const auto first = runtime.prepare(1920, 1080);
    const auto second = runtime.prepare(1920, 1080);

    if (first.empty() || second.empty()) {
        std::cerr << "overlay runtime should render a stable non-empty frame\n";
        return false;
    }
    if (!first.changed || first.sequence == 0) {
        std::cerr << "first prepared frame should be marked changed\n";
        return false;
    }
    if (second.changed) {
        std::cerr << "unchanged prepared frame should not force a texture upload\n";
        return false;
    }
    if (second.sequence != first.sequence) {
        std::cerr << "unchanged prepared frame should keep the upload sequence stable\n";
        return false;
    }

    return true;
}

} // namespace

int main() {
    if (!test_prepare_marks_unchanged_frames_clean()) {
        return 1;
    }
    return 0;
}
