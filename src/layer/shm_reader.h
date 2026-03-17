#ifndef SIGAW_SHM_READER_H
#define SIGAW_SHM_READER_H

#include "../common/protocol.h"
#include "../common/config.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <cstdio>
#include <atomic>
#include <chrono>

namespace sigaw {

class ShmReader {
public:
    explicit ShmReader(std::chrono::milliseconds probe_interval = std::chrono::milliseconds(250))
        : probe_interval_(probe_interval) {}
    ~ShmReader() { close(); }

    /* Non-copyable */
    ShmReader(const ShmReader&) = delete;
    ShmReader& operator=(const ShmReader&) = delete;

    bool open() {
        return refresh_mapping(true);
    }

    void close() {
        if (mapped_ && state_) {
            munmap(const_cast<SigawState*>(state_), sizeof(SigawState));
            state_ = nullptr;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        mapped_ = false;
        stat_valid_ = false;
        state_ = nullptr;
        last_seq_ = 0;
    }

    /*
     * Read current voice state with seqlock consistency.
     * Returns true if state was read successfully (and is valid).
     * The caller-provided `out` is filled with a consistent snapshot.
     */
    bool read(SigawState& out) {
        if (!refresh_mapping(false)) {
            return false;
        }

        /* Validate magic and version */
        if (state_->header.magic != SIGAW_MAGIC) return false;
        if (state_->header.version != SIGAW_VERSION) {
            if (!version_warned_) {
                fprintf(stderr,
                    "[sigaw] Protocol version mismatch: daemon=v%u layer=v%u -- restart sigaw-daemon\n",
                    state_->header.version, (uint32_t)SIGAW_VERSION);
                version_warned_ = true;
            }
            return false;
        }

        /* Seqlock: try up to 3 times */
        for (int attempt = 0; attempt < 3; attempt++) {
            /* Acquire-load the sequence number */
            uint64_t seq1 = __atomic_load_n(&state_->header.sequence,
                                             __ATOMIC_ACQUIRE);

            /* If sequence is odd, a write is in progress -- spin */
            if (seq1 & 1) continue;

            /* Copy the state */
            memcpy(&out, state_, sizeof(SigawState));

            /* Fence + re-read sequence */
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            uint64_t seq2 = __atomic_load_n(&state_->header.sequence,
                                             __ATOMIC_ACQUIRE);

            if (seq1 == seq2) {
                last_seq_ = seq1;
                return true;
            }
        }

        return false;  /* All attempts failed (heavy write contention, unlikely) */
    }

    /*
     * Check if state has changed since last successful read.
     * Useful to skip re-rendering if nothing changed.
     */
    bool has_changed() const {
        if (!mapped_ || !state_) return false;
        uint64_t seq = __atomic_load_n(&state_->header.sequence,
                                        __ATOMIC_RELAXED);
        return seq != last_seq_;
    }

    bool is_connected() const { return mapped_; }

private:
    bool same_object(const struct stat& a, const struct stat& b) const {
        return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
    }

    bool adopt_mapping(int fd, const struct stat& st) {
        void* ptr = mmap(nullptr, sizeof(SigawState), PROT_READ,
                         MAP_SHARED, fd, 0);
        if (ptr == MAP_FAILED) {
            ::close(fd);
            return false;
        }

        close();

        fd_ = fd;
        state_ = static_cast<const SigawState*>(ptr);
        mapped_ = true;
        mapped_stat_ = st;
        stat_valid_ = true;
        version_warned_ = false;
        last_seq_ = 0;
        return true;
    }

    bool refresh_mapping(bool force_probe) {
        const auto now = std::chrono::steady_clock::now();
        if (!force_probe && mapped_ && now < next_probe_) {
            return true;
        }

        next_probe_ = now + probe_interval_;

        const auto shm_name = Config::shared_memory_name();
        const int probe_fd = shm_open(shm_name.c_str(), O_RDONLY, 0);
        if (probe_fd < 0) {
            if (mapped_) {
                close();
            }
            return false;
        }

        struct stat probe_stat = {};
        if (fstat(probe_fd, &probe_stat) < 0) {
            ::close(probe_fd);
            if (mapped_) {
                close();
            }
            return false;
        }

        if (mapped_ && stat_valid_ && same_object(mapped_stat_, probe_stat)) {
            ::close(probe_fd);
            return true;
        }

        return adopt_mapping(probe_fd, probe_stat);
    }

    int                  fd_            = -1;
    const SigawState*    state_         = nullptr;
    bool                 mapped_        = false;
    bool                 stat_valid_    = false;
    bool                 version_warned_ = false;
    uint64_t             last_seq_      = 0;
    struct stat          mapped_stat_   = {};
    std::chrono::milliseconds probe_interval_;
    std::chrono::steady_clock::time_point next_probe_ = {};
};

} /* namespace sigaw */

#endif /* SIGAW_SHM_READER_H */
