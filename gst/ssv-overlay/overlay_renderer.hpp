#pragma once

#include "ssv_meta.hpp"

#include <gst/video/video.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

struct OverlayRenderStats {
    std::size_t drawn_boxes = 0;
    std::size_t skipped_small_boxes = 0;
    bool unsupported_format = false;
};

class OverlayRenderer {
public:
    OverlayRenderer() = default;
    OverlayRenderer(std::string_view font_face, std::uint32_t font_size);

    OverlayRenderStats render(
        GstVideoFrame &video_frame,
        const SsvOverlayFrame &overlay_frame) const;

private:
    bool bold_font_ = false;
    std::uint32_t font_size_ = 7;
};

bool ssv_overlay_glyph_is_supported(char character);
