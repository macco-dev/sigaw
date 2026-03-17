#ifndef SIGAW_CONFIG_WATCHER_H
#define SIGAW_CONFIG_WATCHER_H

#include "config.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>

#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

namespace sigaw {

class ConfigWatcher {
public:
    explicit ConfigWatcher(std::chrono::milliseconds poll_interval = std::chrono::milliseconds(1000))
        : poll_interval_(poll_interval) {}

    ~ConfigWatcher() { close(); }

    ConfigWatcher(const ConfigWatcher&) = delete;
    ConfigWatcher& operator=(const ConfigWatcher&) = delete;

    void sync() {
        close_watch();
        path_ = Config::config_path();
        file_name_ = path_.filename().string();
        signature_ = current_signature();
        initialized_ = true;
        watch_state_ = WatchState::Uninitialized;
    }

    bool consume_change() {
        if (!initialized_) {
            sync();
            return true;
        }

        if (path_ != Config::config_path()) {
            sync();
            return true;
        }

        const bool watch_changed = drain_watch_events();

        const auto now = std::chrono::steady_clock::now();
        if (watch_changed || now >= next_poll_) {
            next_poll_ = now + poll_interval_;

            const Signature current = current_signature();
            if (watch_changed || current != signature_) {
                signature_ = current;
                return true;
            }
        }

        return false;
    }

private:
    struct Signature {
        bool  exists = false;
        dev_t device = 0;
        ino_t inode = 0;
        off_t size = 0;
        long  mtime_sec = 0;
        long  mtime_nsec = 0;

        bool operator==(const Signature& other) const {
            return exists == other.exists &&
                   device == other.device &&
                   inode == other.inode &&
                   size == other.size &&
                   mtime_sec == other.mtime_sec &&
                   mtime_nsec == other.mtime_nsec;
        }

        bool operator!=(const Signature& other) const {
            return !(*this == other);
        }
    };

    enum class WatchState {
        Uninitialized,
        Active,
        Unavailable,
    };

    Signature current_signature() const {
        Signature sig;
        const auto path_string = path_.string();
        struct stat st = {};
        if (::stat(path_string.c_str(), &st) != 0) {
            return sig;
        }

        sig.exists = true;
        sig.device = st.st_dev;
        sig.inode = st.st_ino;
        sig.size = st.st_size;
        sig.mtime_sec = st.st_mtim.tv_sec;
        sig.mtime_nsec = st.st_mtim.tv_nsec;
        return sig;
    }

    void close_watch() {
        if (watch_fd_ >= 0) {
            if (watch_id_ >= 0) {
                ::inotify_rm_watch(watch_fd_, watch_id_);
                watch_id_ = -1;
            }
            ::close(watch_fd_);
            watch_fd_ = -1;
        }
        watched_dir_.clear();
        watch_state_ = WatchState::Uninitialized;
    }

    void close() {
        close_watch();
        initialized_ = false;
    }

    bool ensure_watch() {
        if (watch_state_ == WatchState::Active) {
            return true;
        }
        if (watch_state_ == WatchState::Unavailable) {
            return false;
        }

        const auto dir = path_.parent_path();
        std::error_code ec;
        if (dir.empty() || !std::filesystem::exists(dir, ec) || ec) {
            watch_state_ = WatchState::Unavailable;
            return false;
        }

        watch_fd_ = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (watch_fd_ < 0) {
            watch_state_ = WatchState::Unavailable;
            return false;
        }

        watched_dir_ = dir;
        watch_id_ = ::inotify_add_watch(
            watch_fd_,
            watched_dir_.c_str(),
            IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_ATTRIB |
            IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED
        );
        if (watch_id_ < 0) {
            close_watch();
            watch_state_ = WatchState::Unavailable;
            return false;
        }

        watch_state_ = WatchState::Active;
        return true;
    }

    bool drain_watch_events() {
        if (!ensure_watch()) {
            return false;
        }

        bool changed = false;
        char buffer[4096];
        while (true) {
            const ssize_t n = ::read(watch_fd_, buffer, sizeof(buffer));
            if (n <= 0) {
                break;
            }

            ssize_t offset = 0;
            while (offset < n) {
                const auto* event =
                    reinterpret_cast<const struct inotify_event*>(buffer + offset);

                if (event->mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF)) {
                    close_watch();
                    changed = true;
                    break;
                }

                const bool relevant_name =
                    event->len > 0 && file_name_ == std::string(event->name);
                if (relevant_name &&
                    (event->mask & (IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE |
                                    IN_ATTRIB | IN_DELETE))) {
                    changed = true;
                }

                offset += sizeof(struct inotify_event) + event->len;
            }
        }

        return changed;
    }

    std::chrono::milliseconds            poll_interval_;
    std::chrono::steady_clock::time_point next_poll_ = {};
    std::filesystem::path                path_;
    std::string                          file_name_;
    std::string                          watched_dir_;
    Signature                            signature_{};
    int                                  watch_fd_ = -1;
    int                                  watch_id_ = -1;
    bool                                 initialized_ = false;
    WatchState                           watch_state_ = WatchState::Uninitialized;
};

} /* namespace sigaw */

#endif /* SIGAW_CONFIG_WATCHER_H */
