#ifndef SIGAW_OVERLAY_RUNTIME_H
#define SIGAW_OVERLAY_RUNTIME_H

#include <cstdint>
#include <memory>

#include "overlay_preview.h"

namespace sigaw::overlay {

struct PreparedFrame {
    sigaw::preview::Image image;
    sigaw::preview::Placement placement;

    bool empty() const {
        return image.width == 0 || image.height == 0 || image.rgba.empty();
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
