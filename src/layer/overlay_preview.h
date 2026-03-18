#ifndef SIGAW_OVERLAY_PREVIEW_H
#define SIGAW_OVERLAY_PREVIEW_H

#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <vector>

#include "../common/config.h"
#include "../common/protocol.h"

namespace sigaw::preview {

struct Image {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;
};

struct Placement {
    int x = 0;
    int y = 0;
};

bool render_panel_rgba(const SigawState& state, const sigaw::Config& cfg,
                       const std::unordered_map<uint64_t, float>& speaking_times_ms,
                       Image& out);

Placement place_panel(const sigaw::Config& cfg, uint32_t panel_w, uint32_t panel_h,
                      uint32_t screen_w, uint32_t screen_h);

bool write_png(const std::filesystem::path& path, const Image& image);

} /* namespace sigaw::preview */

#endif /* SIGAW_OVERLAY_PREVIEW_H */
