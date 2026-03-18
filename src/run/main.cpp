#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

#include "launcher_env.h"

namespace {

void print_usage(const char* prog) {
    fprintf(stderr,
        "sigaw-run - Launch an application with Sigaw enabled\n"
        "\n"
        "Usage: %s [--] command [args...]\n"
        "\n"
        "This enables the Vulkan implicit layer (`SIGAW=1`) and prepends\n"
        "Sigaw's OpenGL preload library to `LD_PRELOAD` before exec.\n",
        prog
    );
}

std::string preload_library_path() {
    if (const char* override_path = std::getenv("SIGAW_GL_PRELOAD"); override_path && *override_path) {
        return override_path;
    }
    return SIGAW_GL_PRELOAD_PATH;
}

} /* namespace */

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    if (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "help") == 0) {
        print_usage(argv[0]);
        return 0;
    }

    int command_index = 1;
    if (std::strcmp(argv[1], "--") == 0) {
        command_index = 2;
    }

    if (command_index >= argc) {
        print_usage(argv[0]);
        return 1;
    }

    const std::string library_path = preload_library_path();
    const char* existing_preload = std::getenv("LD_PRELOAD");
    const std::string merged_preload =
        sigaw::run::prepend_preload(existing_preload ? existing_preload : "", library_path);

    setenv("SIGAW", "1", 1);
    unsetenv("DISABLE_SIGAW");
    if (!merged_preload.empty()) {
        setenv("LD_PRELOAD", merged_preload.c_str(), 1);
    }

    execvp(argv[command_index], &argv[command_index]);
    fprintf(stderr, "sigaw-run: failed to exec %s: %s\n", argv[command_index], std::strerror(errno));
    return 127;
}
