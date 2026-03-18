#ifndef SIGAW_OVERLAY_RUNTIME_H
#define SIGAW_OVERLAY_RUNTIME_H

#include <cstddef>
#include <cstdint>
#include <memory>

#include "overlay_preview.h"

namespace sigaw::overlay {

struct PreparedFrame {
    const uint8_t* rgba = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    size_t byte_size = 0;
    sigaw::preview::Placement placement;
    uint64_t sequence = 0;
    bool changed = false;

    bool empty() const {
        return rgba == nullptr || width == 0 || height == 0 || byte_size == 0;
    }
};

class Runtime {
public:
    Runtime();
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    Runtime(Runtime&&) noexcept;
    Runtime& operator=(Runtime&&) noexcept;

    PreparedFrame prepare(uint32_t surface_width, uint32_t surface_height);
    bool debug_enabled() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} /* namespace sigaw::overlay */

#endif /* SIGAW_OVERLAY_RUNTIME_H */
