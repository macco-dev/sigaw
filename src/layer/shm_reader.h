#ifndef SIGAW_SHM_READER_H
#define SIGAW_SHM_READER_H

#include "../common/protocol.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <cstdio>
#include <atomic>

namespace sigaw {

class ShmReader {
public:
    ShmReader() = default;
    ~ShmReader() { close(); }

    /* Non-copyable */
    ShmReader(const ShmReader&) = delete;
    ShmReader& operator=(const ShmReader&) = delete;

    bool open() {
        if (mapped_) return true;

        fd_ = shm_open(SIGAW_SHM_NAME, O_RDONLY, 0);
        if (fd_ < 0) return false;

        void* ptr = mmap(nullptr, sizeof(SigawState), PROT_READ,
                         MAP_SHARED, fd_, 0);
        if (ptr == MAP_FAILED) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }

        state_ = static_cast<const SigawState*>(ptr);
        mapped_ = true;
        return true;
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
    }

    /*
     * Read current voice state with seqlock consistency.
     * Returns true if state was read successfully (and is valid).
     * The caller-provided `out` is filled with a consistent snapshot.
     */
    bool read(SigawState& out) {
        if (!mapped_) {
            /* Try to connect on each read attempt (daemon may start later) */
            if (!open()) return false;
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
    int                  fd_            = -1;
    const SigawState*    state_         = nullptr;
    bool                 mapped_        = false;
    bool                 version_warned_ = false;
    uint64_t             last_seq_      = 0;
};

} /* namespace sigaw */

#endif /* SIGAW_SHM_READER_H */
