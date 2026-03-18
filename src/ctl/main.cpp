#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <string>

#include "../common/protocol.h"
#include "../common/config.h"

static void print_usage(const char* prog) {
    fprintf(stderr,
        "sigaw-ctl - Sigaw overlay control\n"
        "\n"
        "Usage: %s <command>\n"
        "\n"
        "Commands:\n"
        "  status     Show current overlay and voice state\n"
        "  toggle     Toggle overlay visibility\n"
        "  reload     Reload configuration\n"
        "  stop       Stop the daemon\n"
        "  config     Print config file path\n"
        "\n", prog
    );
}

static const SigawState* open_shm() {
    const auto shm_name = sigaw::Config::shared_memory_name();
    int fd = shm_open(shm_name.c_str(), O_RDONLY, 0);
    if (fd < 0) return nullptr;

    void* ptr = mmap(nullptr, sizeof(SigawState), PROT_READ,
                     MAP_SHARED, fd, 0);
    close(fd);

    if (ptr == MAP_FAILED) return nullptr;
    return static_cast<const SigawState*>(ptr);
}

static int send_ctl_request(uint32_t command, std::string* message = nullptr) {
    const auto socket_path = sigaw::Config::control_socket_path();
    const auto path = socket_path.string();

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        fprintf(stderr, "Control socket path too long: %s\n", path.c_str());
        close(fd);
        return 1;
    }
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        fprintf(stderr, "Failed to connect to %s: %s\n",
                path.c_str(), strerror(errno));
        close(fd);
        return 1;
    }

    SigawCtlRequest request = {};
    request.command = command;
    size_t sent = 0;
    const auto* in = reinterpret_cast<const char*>(&request);
    while (sent < sizeof(request)) {
        const ssize_t w = write(fd, in + sent, sizeof(request) - sent);
        if (w <= 0) {
            perror("write");
            close(fd);
            return 1;
        }
        sent += static_cast<size_t>(w);
    }

    SigawCtlResponse response = {};
    ssize_t got = 0;
    auto* out = reinterpret_cast<char*>(&response);
    while (got < static_cast<ssize_t>(sizeof(response))) {
        const ssize_t r = read(fd, out + got, sizeof(response) - got);
        if (r <= 0) {
            perror("read");
            close(fd);
            return 1;
        }
        got += r;
    }

    close(fd);

    if (message) {
        *message = response.message;
    }
    if (response.status != 0) {
        fprintf(stderr, "%s\n", response.message);
        return 1;
    }
    return 0;
}

static int cmd_status() {
    const SigawState* state = open_shm();

    if (!state || state->header.magic != SIGAW_MAGIC) {
        if (state) {
            munmap(const_cast<SigawState*>(state), sizeof(SigawState));
        }
        printf("Sigaw daemon: not running\n");
        return 1;
    }

    printf("Sigaw daemon: running (v%u, seq %llu)\n",
           state->header.version,
           (unsigned long long)state->header.sequence);

    if (state->header.channel_name_len > 0) {
        printf("Voice channel: %s\n", state->header.channel_name);
        printf("Users (%u):\n", state->user_count);

        for (uint32_t i = 0; i < state->user_count; i++) {
            const SigawUser* u = &state->users[i];
            char status[64] = "";

            if (u->speaking)    strcat(status, " [SPEAKING]");
            if (u->self_mute)   strcat(status, " [muted]");
            if (u->self_deaf)   strcat(status, " [deafened]");
            if (u->server_mute) strcat(status, " [server muted]");
            if (u->server_deaf) strcat(status, " [server deafened]");

            printf("  %s%s\n", u->username, status);
        }

        if (state->chat_count > 0) {
            printf("Recent chat (%u):\n", state->chat_count);
            for (uint32_t i = 0; i < state->chat_count; ++i) {
                const SigawChatMessage* message = &state->chat_messages[i];
                printf("  %s: %s\n", message->author_name, message->content);
            }
        }
    } else {
        printf("Voice channel: none\n");
    }

    sigaw::Config cfg = sigaw::Config::load();
    printf("\nOverlay: %s, ", cfg.visible ? "visible" : "hidden");
    const char* pos_names[] = {"top-left", "top-right", "bottom-left", "bottom-right"};
    printf("%s, %.0f%% opacity\n",
           pos_names[(int)cfg.position],
           cfg.opacity * 100.0f);

    munmap(const_cast<SigawState*>(state), sizeof(SigawState));
    return 0;
}

static int cmd_config() {
    auto path = sigaw::Config::config_path();
    printf("%s\n", path.c_str());
    return 0;
}

static int cmd_toggle() {
    std::string message;
    const int rc = send_ctl_request(SIGAW_CTL_TOGGLE, &message);
    if (rc == 0 && !message.empty()) {
        printf("%s\n", message.c_str());
    }
    return rc;
}

static int cmd_reload() {
    std::string message;
    const int rc = send_ctl_request(SIGAW_CTL_RELOAD, &message);
    if (rc == 0 && !message.empty()) {
        printf("%s\n", message.c_str());
    }
    return rc;
}

static int cmd_stop() {
    std::string message;
    const int rc = send_ctl_request(SIGAW_CTL_QUIT, &message);
    if (rc == 0 && !message.empty()) {
        printf("%s\n", message.c_str());
    }
    return rc;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char* cmd = argv[1];

    if (strcmp(cmd, "status") == 0)  return cmd_status();
    if (strcmp(cmd, "config") == 0)  return cmd_config();
    if (strcmp(cmd, "toggle") == 0)  return cmd_toggle();
    if (strcmp(cmd, "reload") == 0)  return cmd_reload();
    if (strcmp(cmd, "stop") == 0)    return cmd_stop();
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0) {
        print_usage(argv[0]);
        return 0;
    }

    fprintf(stderr, "Unknown command: %s\n", cmd);
    print_usage(argv[0]);
    return 1;
}
