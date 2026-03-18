#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

#include "common/config.h"
#include "layer/vk_dispatch.h"
#include "layer/wine_policy.h"

namespace {

VKAPI_ATTR void VKAPI_CALL dummy_vk_entry(void) {}

bool g_omit_sampler = false;

PFN_vkVoidFunction fake_get_instance_proc(VkInstance, const char* name) {
    if (std::strcmp(name, "vkGetPhysicalDeviceMemoryProperties") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(&dummy_vk_entry);
    }
    return nullptr;
}

PFN_vkVoidFunction fake_get_device_proc(VkDevice, const char* name) {
#define SIGAW_MATCH_DEVICE_PROC(type, field, symbol)              \
    if (std::strcmp(name, symbol) == 0) {                         \
        if (g_omit_sampler && std::strcmp(name, "vkCreateSampler") == 0) { \
            return nullptr;                                       \
        }                                                         \
        return reinterpret_cast<PFN_vkVoidFunction>(&dummy_vk_entry); \
    }
    SIGAW_VK_DEVICE_DISPATCH_FUNCTIONS(SIGAW_MATCH_DEVICE_PROC)
#undef SIGAW_MATCH_DEVICE_PROC

    return nullptr;
}

struct ScopedEnv {
    ScopedEnv(const char* name, const char* value) : name_(name) {
        const char* existing = std::getenv(name_);
        if (existing) {
            had_value_ = true;
            previous_ = existing;
        }

        if (value) {
            setenv(name_, value, 1);
        } else {
            unsetenv(name_);
        }
    }

    ~ScopedEnv() {
        if (had_value_) {
            setenv(name_, previous_.c_str(), 1);
        } else {
            unsetenv(name_);
        }
    }

    const char* name_;
    bool had_value_ = false;
    std::string previous_;
};

bool test_wine_policy_defaults() {
    ScopedEnv compat("STEAM_COMPAT_DATA_PATH", nullptr);
    ScopedEnv prefix("WINEPREFIX", nullptr);
    ScopedEnv dllpath("WINEDLLPATH", nullptr);
    ScopedEnv policy("SIGAW_WINE_POLICY", nullptr);

    if (sigaw_is_wine_environment()) {
        std::cerr << "wine environment should be false without Wine variables\n";
        return false;
    }
    if (sigaw_wine_policy_from_env() != SIGAW_WINE_POLICY_AUTO) {
        std::cerr << "default wine policy should be auto\n";
        return false;
    }
    return true;
}

bool test_wine_policy_parsing() {
    ScopedEnv compat("STEAM_COMPAT_DATA_PATH", "/tmp/compat");
    ScopedEnv policy("SIGAW_WINE_POLICY", "force");

    if (!sigaw_is_wine_environment()) {
        std::cerr << "steam compat path should mark Wine/Proton environment\n";
        return false;
    }
    if (sigaw_wine_policy_from_env() != SIGAW_WINE_POLICY_FORCE) {
        std::cerr << "force policy should parse case-insensitively\n";
        return false;
    }

    setenv("SIGAW_WINE_POLICY", "disable", 1);
    if (sigaw_wine_policy_from_env() != SIGAW_WINE_POLICY_DISABLE) {
        std::cerr << "disable policy should parse\n";
        return false;
    }

    setenv("SIGAW_WINE_POLICY", "bogus", 1);
    if (sigaw_wine_policy_from_env() != SIGAW_WINE_POLICY_AUTO) {
        std::cerr << "invalid policy should fall back to auto\n";
        return false;
    }

    return true;
}

bool test_current_executable_basename_uses_wine_game_argument() {
    ScopedEnv compat("STEAM_COMPAT_DATA_PATH", "/tmp/compat");

    std::error_code ec;
    const auto self = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec || self.empty()) {
        std::cerr << "failed to resolve test binary path via /proc/self/exe\n";
        return false;
    }

    const pid_t child = fork();
    if (child < 0) {
        std::cerr << "fork failed: " << std::strerror(errno) << "\n";
        return false;
    }

    if (child == 0) {
        execl(
            self.c_str(),
            self.c_str(),
            "--verify-current-exe",
            "Z:\\Games\\TestGame.exe",
            static_cast<char*>(nullptr)
        );
        _exit(127);
    }

    int status = 0;
    if (waitpid(child, &status, 0) < 0) {
        std::cerr << "waitpid failed: " << std::strerror(errno) << "\n";
        return false;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::cerr << "Wine basename child verification failed\n";
        return false;
    }

    return true;
}

bool test_dispatch_resolution_complete() {
    SigawVulkanDispatch dispatch = {};
    g_omit_sampler = false;
    if (!sigaw_vk_dispatch_resolve(&dispatch, fake_get_instance_proc,
                                   (VkInstance)0x1, fake_get_device_proc,
                                   (VkDevice)0x2)) {
        std::cerr << "dispatch resolution should succeed when all entries are present\n";
        return false;
    }
    if (!sigaw_vk_dispatch_complete(&dispatch)) {
        std::cerr << "resolved dispatch should report complete\n";
        return false;
    }
    return true;
}

bool test_dispatch_resolution_missing_entry() {
    SigawVulkanDispatch dispatch = {};
    g_omit_sampler = true;
    if (sigaw_vk_dispatch_resolve(&dispatch, fake_get_instance_proc,
                                  (VkInstance)0x1, fake_get_device_proc,
                                  (VkDevice)0x2)) {
        std::cerr << "dispatch resolution should fail when a required entry is missing\n";
        return false;
    }
    if (sigaw_vk_dispatch_complete(&dispatch)) {
        std::cerr << "incomplete dispatch should not report complete\n";
        return false;
    }
    g_omit_sampler = false;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--verify-current-exe") == 0) {
        const auto detected = sigaw::Config::current_executable_basename();
        if (detected != "TestGame.exe") {
            std::cerr << "expected Wine basename detection to return TestGame.exe, got "
                      << detected << "\n";
            return 1;
        }
        return 0;
    }

    if (!test_wine_policy_defaults()) {
        return 1;
    }
    if (!test_wine_policy_parsing()) {
        return 1;
    }
    if (!test_current_executable_basename_uses_wine_game_argument()) {
        return 1;
    }
    if (!test_dispatch_resolution_complete()) {
        return 1;
    }
    if (!test_dispatch_resolution_missing_entry()) {
        return 1;
    }
    return 0;
}
