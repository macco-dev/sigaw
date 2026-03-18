#ifndef SIGAW_CONTROL_SERVER_H
#define SIGAW_CONTROL_SERVER_H

#include "../common/config.h"
#include "../common/protocol.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace sigaw {

class ControlServer {
public:
    enum class Action {
        None,
        Refresh,
        Reload,
        Quit,
    };

    ControlServer() = default;
    ~ControlServer() { close(); }

    bool open() {
        if (fd_ >= 0) return true;

        socket_path_ = Config::control_socket_path();

        std::error_code ec;
        std::filesystem::create_directories(socket_path_.parent_path(), ec);
        if (ec) {
            fprintf(stderr, "[sigaw] Failed to create control socket directory: %s\n",
                    ec.message().c_str());
            return false;
        }

        std::filesystem::remove(socket_path_, ec);

        fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd_ < 0) {
            perror("[sigaw] socket");
            return false;
        }

        int flags = fcntl(fd_, F_GETFL, 0);
        if (flags >= 0) {
            (void)fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
        }

        sockaddr_un addr = {};
        addr.sun_family = AF_UNIX;
        const auto socket_path = socket_path_.string();
        if (socket_path.size() >= sizeof(addr.sun_path)) {
            fprintf(stderr, "[sigaw] Control socket path too long: %s\n",
                    socket_path.c_str());
            close();
            return false;
        }
        strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            perror("[sigaw] bind");
            close();
            return false;
        }

        std::filesystem::permissions(
            socket_path_,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace,
            ec
        );

        if (::listen(fd_, 8) < 0) {
            perror("[sigaw] listen");
            close();
            return false;
        }

        return true;
    }

    void close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }

        if (!socket_path_.empty()) {
            std::error_code ec;
            std::filesystem::remove(socket_path_, ec);
            socket_path_.clear();
        }
    }

    Action process_pending(Config& config) {
        if (fd_ < 0) return Action::None;

        Action action = Action::None;
        while (true) {
            int client = ::accept(fd_, nullptr, nullptr);
            if (client < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                perror("[sigaw] accept");
                break;
            }

            const auto client_action = handle_client(client, config);
            ::close(client);

            if (client_action == Action::Quit) {
                return Action::Quit;
            }
            if (client_action == Action::Reload) {
                action = Action::Reload;
            } else if (client_action == Action::Refresh && action == Action::None) {
                action = Action::Refresh;
            }
        }

        return action;
    }

    int fd() const { return fd_; }

private:
    int                   fd_ = -1;
    std::filesystem::path socket_path_;

    static ssize_t read_all(int fd, void* buf, size_t len) {
        size_t got = 0;
        auto* out = static_cast<uint8_t*>(buf);
        while (got < len) {
            const ssize_t r = ::read(fd, out + got, len - got);
            if (r <= 0) return r;
            got += static_cast<size_t>(r);
        }
        return static_cast<ssize_t>(got);
    }

    static ssize_t write_all(int fd, const void* buf, size_t len) {
        size_t sent = 0;
        const auto* in = static_cast<const uint8_t*>(buf);
        while (sent < len) {
            const ssize_t w = ::write(fd, in + sent, len - sent);
            if (w <= 0) return w;
            sent += static_cast<size_t>(w);
        }
        return static_cast<ssize_t>(sent);
    }

    static bool send_response(int fd, uint32_t command, uint32_t status,
                              const std::string& message)
    {
        SigawCtlResponse response = {};
        response.command = command;
        response.status = status;
        strncpy(response.message, message.c_str(), sizeof(response.message) - 1);
        return write_all(fd, &response, sizeof(response)) == sizeof(response);
    }

    static Action handle_client(int client, Config& config) {
        SigawCtlRequest request = {};
        if (read_all(client, &request, sizeof(request)) != sizeof(request)) {
            send_response(client, 0, 1, "invalid request");
            return Action::None;
        }

        switch (request.command) {
            case SIGAW_CTL_STATUS:
                send_response(client, request.command, 0, "ok");
                return Action::None;

            case SIGAW_CTL_TOGGLE:
                config.visible = !config.visible;
                if (!config.save()) {
                    send_response(client, request.command, 1,
                                  "failed to persist config");
                    return Action::None;
                }
                send_response(
                    client,
                    request.command,
                    0,
                    config.visible ? "overlay visible" : "overlay hidden"
                );
                return Action::Refresh;

            case SIGAW_CTL_RELOAD:
                config = Config::load();
                send_response(client, request.command, 0, "configuration reloaded");
                return Action::Reload;

            case SIGAW_CTL_QUIT:
                send_response(client, request.command, 0, "daemon stopping");
                return Action::Quit;

            default:
                send_response(client, request.command, 1, "unknown command");
                return Action::None;
        }
    }
};

} /* namespace sigaw */

#endif /* SIGAW_CONTROL_SERVER_H */
