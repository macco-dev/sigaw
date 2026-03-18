#ifndef SIGAW_OVERLAY_FRAME_SHM_H
#define SIGAW_OVERLAY_FRAME_SHM_H

#include "config.h"
#include "protocol.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

namespace sigaw {

struct OverlayFrameSnapshot {
    bool            visible = false;
    OverlayPosition position = OverlayPosition::TopRight;
    uint32_t        margin_px = 0;
    uint32_t        width = 0;
    uint32_t        height = 0;
    uint32_t        stride = 0;
    uint64_t        sequence = 0;
    std::vector<uint8_t> rgba;

    bool empty() const {
        return !visible || width == 0 || height == 0 || rgba.empty();
    }
};

static inline uint32_t overlay_anchor(OverlayPosition position) {
    switch (position) {
        case OverlayPosition::TopLeft:
            return SIGAW_OVERLAY_ANCHOR_TOP_LEFT;
        case OverlayPosition::TopRight:
            return SIGAW_OVERLAY_ANCHOR_TOP_RIGHT;
        case OverlayPosition::BottomLeft:
            return SIGAW_OVERLAY_ANCHOR_BOTTOM_LEFT;
        case OverlayPosition::BottomRight:
            return SIGAW_OVERLAY_ANCHOR_BOTTOM_RIGHT;
    }
    return SIGAW_OVERLAY_ANCHOR_TOP_RIGHT;
}

static inline OverlayPosition overlay_position(uint32_t anchor) {
    switch (anchor) {
        case SIGAW_OVERLAY_ANCHOR_TOP_LEFT:
            return OverlayPosition::TopLeft;
        case SIGAW_OVERLAY_ANCHOR_TOP_RIGHT:
            return OverlayPosition::TopRight;
        case SIGAW_OVERLAY_ANCHOR_BOTTOM_LEFT:
            return OverlayPosition::BottomLeft;
        case SIGAW_OVERLAY_ANCHOR_BOTTOM_RIGHT:
            return OverlayPosition::BottomRight;
        default:
            return OverlayPosition::TopRight;
    }
}

class OverlayFrameWriter {
public:
    OverlayFrameWriter() = default;
    ~OverlayFrameWriter() { close(); }

    OverlayFrameWriter(const OverlayFrameWriter&) = delete;
    OverlayFrameWriter& operator=(const OverlayFrameWriter&) = delete;

    bool publish(const OverlayFrameSnapshot& frame) {
        const size_t byte_size = frame.visible ? frame.rgba.size() : 0u;
        const size_t required_size = sizeof(SigawOverlayFrameHeader) + byte_size;
        if (!ensure_mapping(required_size)) {
            return false;
        }

        if (same_payload(frame, byte_size)) {
            return true;
        }

        auto* header = static_cast<SigawOverlayFrameHeader*>(mapping_);
        const uint64_t write_sequence = sequence_ + 1u;
        __atomic_store_n(&header->sequence, write_sequence, __ATOMIC_RELEASE);
        __atomic_thread_fence(__ATOMIC_RELEASE);

        header->magic = SIGAW_OVERLAY_MAGIC;
        header->version = SIGAW_VERSION;
        header->visible = frame.visible ? 1u : 0u;
        header->anchor = overlay_anchor(frame.position);
        header->margin_px = frame.margin_px;
        header->width = frame.visible ? frame.width : 0u;
        header->height = frame.visible ? frame.height : 0u;
        header->stride = frame.visible ? frame.stride : 0u;
        header->byte_size = static_cast<uint32_t>(byte_size);
        std::memset(header->_pad, 0, sizeof(header->_pad));

        auto* pixels = static_cast<uint8_t*>(mapping_) + sizeof(SigawOverlayFrameHeader);
        if (byte_size != 0u) {
            std::memcpy(pixels, frame.rgba.data(), byte_size);
        }

        sequence_ += 2u;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        __atomic_store_n(&header->sequence, sequence_, __ATOMIC_RELEASE);
        return true;
    }

    void close() {
        if (mapping_ != nullptr) {
            munmap(mapping_, mapped_size_);
            mapping_ = nullptr;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        if (mapped_size_ != 0u) {
            const auto shm_name = Config::overlay_frame_memory_name();
            shm_unlink(shm_name.c_str());
        }
        mapped_size_ = 0;
    }

private:
    bool ensure_mapping(size_t required_size) {
        if (mapping_ != nullptr && mapped_size_ == required_size) {
            return true;
        }

        close();

        const auto shm_name = Config::overlay_frame_memory_name();
        fd_ = shm_open(shm_name.c_str(), O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
        if (fd_ < 0) {
            perror("[sigaw] overlay shm_open");
            return false;
        }

        if (ftruncate(fd_, static_cast<off_t>(required_size)) < 0) {
            perror("[sigaw] overlay ftruncate");
            ::close(fd_);
            fd_ = -1;
            return false;
        }

        mapping_ = mmap(nullptr, required_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (mapping_ == MAP_FAILED) {
            perror("[sigaw] overlay mmap");
            mapping_ = nullptr;
            ::close(fd_);
            fd_ = -1;
            return false;
        }

        mapped_size_ = required_size;
        auto* header = static_cast<SigawOverlayFrameHeader*>(mapping_);
        std::memset(header, 0, sizeof(*header));
        header->magic = SIGAW_OVERLAY_MAGIC;
        header->version = SIGAW_VERSION;
        header->sequence = sequence_;
        return true;
    }

    bool same_payload(const OverlayFrameSnapshot& frame, size_t byte_size) const {
        if (mapping_ == nullptr ||
            mapped_size_ != sizeof(SigawOverlayFrameHeader) + byte_size) {
            return false;
        }

        const auto* header = static_cast<const SigawOverlayFrameHeader*>(mapping_);
        if (header->magic != SIGAW_OVERLAY_MAGIC ||
            header->version != SIGAW_VERSION ||
            header->visible != (frame.visible ? 1u : 0u) ||
            header->anchor != overlay_anchor(frame.position) ||
            header->margin_px != frame.margin_px ||
            header->width != (frame.visible ? frame.width : 0u) ||
            header->height != (frame.visible ? frame.height : 0u) ||
            header->stride != (frame.visible ? frame.stride : 0u) ||
            header->byte_size != byte_size) {
            return false;
        }

        if (byte_size == 0u) {
            return true;
        }

        const auto* pixels =
            static_cast<const uint8_t*>(mapping_) + sizeof(SigawOverlayFrameHeader);
        return std::memcmp(pixels, frame.rgba.data(), byte_size) == 0;
    }

    int    fd_ = -1;
    void*  mapping_ = nullptr;
    size_t mapped_size_ = 0;
    uint64_t sequence_ = 0;
};

class OverlayFrameReader {
public:
    explicit OverlayFrameReader(
        std::chrono::milliseconds probe_interval = std::chrono::milliseconds(250)
    )
        : probe_interval_(probe_interval) {}

    ~OverlayFrameReader() { close(); }

    OverlayFrameReader(const OverlayFrameReader&) = delete;
    OverlayFrameReader& operator=(const OverlayFrameReader&) = delete;

    bool read(OverlayFrameSnapshot& out, bool* changed = nullptr) {
        if (changed) {
            *changed = false;
        }

        if (!refresh_mapping(false)) {
            return false;
        }

        const auto* header = static_cast<const SigawOverlayFrameHeader*>(mapping_);
        if (header->magic != SIGAW_OVERLAY_MAGIC) {
            return false;
        }
        if (header->version != SIGAW_VERSION) {
            if (!version_warned_) {
                fprintf(stderr,
                        "[sigaw] Overlay protocol version mismatch: daemon=v%u layer=v%u\n",
                        header->version, (uint32_t)SIGAW_VERSION);
                version_warned_ = true;
            }
            return false;
        }

        for (int attempt = 0; attempt < 3; ++attempt) {
            const uint64_t seq1 =
                __atomic_load_n(&header->sequence, __ATOMIC_ACQUIRE);
            if ((seq1 & 1u) != 0u) {
                continue;
            }

            if (seq1 == last_seq_) {
                return true;
            }

            SigawOverlayFrameHeader snapshot = *header;
            const size_t byte_size = snapshot.visible ? snapshot.byte_size : 0u;
            if (sizeof(SigawOverlayFrameHeader) + byte_size > mapped_size_) {
                next_probe_ = {};
                return false;
            }

            out.visible = snapshot.visible != 0u;
            out.position = overlay_position(snapshot.anchor);
            out.margin_px = snapshot.margin_px;
            out.width = snapshot.visible ? snapshot.width : 0u;
            out.height = snapshot.visible ? snapshot.height : 0u;
            out.stride = snapshot.visible ? snapshot.stride : 0u;
            out.rgba.resize(byte_size);
            if (byte_size != 0u) {
                const auto* pixels =
                    static_cast<const uint8_t*>(mapping_) + sizeof(SigawOverlayFrameHeader);
                std::memcpy(out.rgba.data(), pixels, byte_size);
            }

            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            const uint64_t seq2 =
                __atomic_load_n(&header->sequence, __ATOMIC_ACQUIRE);
            if (seq1 == seq2) {
                last_seq_ = seq1;
                out.sequence = seq1;
                if (changed) {
                    *changed = true;
                }
                return true;
            }
        }

        return false;
    }

    bool has_changed() const {
        if (mapping_ == nullptr) {
            return false;
        }
        const auto* header = static_cast<const SigawOverlayFrameHeader*>(mapping_);
        const uint64_t seq =
            __atomic_load_n(&header->sequence, __ATOMIC_RELAXED);
        return seq != last_seq_;
    }

    bool is_connected() const { return mapping_ != nullptr; }

    void close() {
        if (mapping_ != nullptr) {
            munmap(mapping_, mapped_size_);
            mapping_ = nullptr;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        mapped_size_ = 0;
        stat_valid_ = false;
        version_warned_ = false;
        last_seq_ = 0;
    }

private:
    bool same_object(const struct stat& a, const struct stat& b) const {
        return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size;
    }

    bool adopt_mapping(int fd, const struct stat& st) {
        void* ptr = mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_SHARED, fd, 0);
        if (ptr == MAP_FAILED) {
            ::close(fd);
            return false;
        }

        close();

        fd_ = fd;
        mapping_ = ptr;
        mapped_size_ = static_cast<size_t>(st.st_size);
        mapped_stat_ = st;
        stat_valid_ = true;
        version_warned_ = false;
        last_seq_ = 0;
        return true;
    }

    bool refresh_mapping(bool force_probe) {
        const auto now = std::chrono::steady_clock::now();
        if (!force_probe && mapping_ != nullptr && now < next_probe_) {
            return true;
        }

        next_probe_ = now + probe_interval_;

        const auto shm_name = Config::overlay_frame_memory_name();
        const int probe_fd = shm_open(shm_name.c_str(), O_RDONLY, 0);
        if (probe_fd < 0) {
            if (mapping_ != nullptr) {
                close();
            }
            return false;
        }

        struct stat st = {};
        if (fstat(probe_fd, &st) < 0 || st.st_size < (off_t)sizeof(SigawOverlayFrameHeader)) {
            ::close(probe_fd);
            if (mapping_ != nullptr) {
                close();
            }
            return false;
        }

        if (mapping_ != nullptr && stat_valid_ && same_object(mapped_stat_, st)) {
            ::close(probe_fd);
            return true;
        }

        return adopt_mapping(probe_fd, st);
    }

    int                  fd_ = -1;
    void*                mapping_ = nullptr;
    size_t               mapped_size_ = 0;
    struct stat          mapped_stat_ = {};
    bool                 stat_valid_ = false;
    bool                 version_warned_ = false;
    uint64_t             last_seq_ = 0;
    std::chrono::milliseconds probe_interval_;
    std::chrono::steady_clock::time_point next_probe_ = {};
};

} /* namespace sigaw */

#endif /* SIGAW_OVERLAY_FRAME_SHM_H */
