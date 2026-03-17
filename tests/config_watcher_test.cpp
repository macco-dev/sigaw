#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

#include "common/config.h"
#include "common/config_watcher.h"

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
    return std::filesystem::temp_directory_path() / ("sigaw-config-watch-" + suffix) / "sigaw.conf";
}

void write_file(const std::filesystem::path& path, const std::string& contents) {
    std::ofstream out(path);
    out << contents;
}

bool wait_for_change(sigaw::ConfigWatcher& watcher,
                     std::chrono::milliseconds timeout = std::chrono::milliseconds(500))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (watcher.consume_change()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

bool test_detects_direct_write() {
    ConfigEnvGuard guard(unique_config_path());
    write_file(guard.path_, "position=top-right\n");

    sigaw::ConfigWatcher watcher(std::chrono::milliseconds(50));
    watcher.sync();

    write_file(guard.path_, "position=bottom-left\n");
    if (!wait_for_change(watcher)) {
        std::cerr << "config watcher did not detect direct file write\n";
        return false;
    }

    return true;
}

bool test_detects_atomic_replace() {
    ConfigEnvGuard guard(unique_config_path());
    write_file(guard.path_, "position=top-right\n");

    sigaw::ConfigWatcher watcher(std::chrono::milliseconds(50));
    watcher.sync();

    const auto tmp = guard.path_.parent_path() / "sigaw.conf.tmp";
    write_file(tmp, "position=bottom-right\n");
    std::filesystem::rename(tmp, guard.path_);

    if (!wait_for_change(watcher)) {
        std::cerr << "config watcher did not detect atomic file replace\n";
        return false;
    }

    return true;
}

} // namespace

int main() {
    if (!test_detects_direct_write()) {
        return 1;
    }

    if (!test_detects_atomic_replace()) {
        return 1;
    }

    return 0;
}
