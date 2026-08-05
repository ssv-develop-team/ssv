#include "overlay_renderer.hpp"

#include <gst/video/video.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string_view>

namespace {

struct RenderedLabel {
    std::size_t white_pixels = 0;
    int height = 0;
};

SsvOverlayFrame make_overlay_frame()
{
    SsvOverlayFrame frame;
    SsvOverlayBox box;
    std::snprintf(
        box.detection.class_name,
        sizeof(box.detection.class_name),
        "person");
    box.detection.confidence = 0.92F;
    box.detection.x1 = 0.05F;
    box.detection.y1 = 0.75F;
    box.detection.x2 = 0.8F;
    box.detection.y2 = 0.95F;
    box.track_id = 5;
    box.track_state = SSV_TRACK_NEW;
    box.occluded = true;
    frame.boxes.push_back(box);
    return frame;
}

RenderedLabel render_label(std::string_view font_face, std::uint32_t font_size)
{
    GstVideoInfo info;
    gst_video_info_init(&info);
    assert(gst_video_info_set_format(
        &info, GST_VIDEO_FORMAT_BGRx, 320, 120));
    GstBuffer *buffer = gst_buffer_new_allocate(
        nullptr, GST_VIDEO_INFO_SIZE(&info), nullptr);
    assert(buffer != nullptr);
    gst_buffer_memset(buffer, 0, 0, GST_VIDEO_INFO_SIZE(&info));

    GstVideoFrame video_frame;
    assert(gst_video_frame_map(
        &video_frame, &info, buffer, GST_MAP_READWRITE));
    OverlayRenderer renderer(font_face, font_size);
    const auto stats = renderer.render(video_frame, make_overlay_frame());
    assert(stats.drawn_boxes == 1);

    int min_y = std::numeric_limits<int>::max();
    int max_y = -1;
    const int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&video_frame, 0);
    const auto *data = static_cast<const std::uint8_t *>(
        GST_VIDEO_FRAME_PLANE_DATA(&video_frame, 0));
    RenderedLabel rendered;
    for (int y = 0; y < GST_VIDEO_FRAME_HEIGHT(&video_frame); ++y) {
        for (int x = 0; x < GST_VIDEO_FRAME_WIDTH(&video_frame); ++x) {
            const auto *pixel = data + y * stride + x * 4;
            if (pixel[0] == 255 && pixel[1] == 255 && pixel[2] == 255) {
                ++rendered.white_pixels;
                min_y = std::min(min_y, y);
                max_y = std::max(max_y, y);
            }
        }
    }
    rendered.height = max_y >= min_y ? max_y - min_y + 1 : 0;

    gst_video_frame_unmap(&video_frame);
    gst_buffer_unref(buffer);
    return rendered;
}

} // namespace

void run_overlay_renderer_contract_tests()
{
    const char *label = "person #5[No] 0.92";
    for (const char *character = label; *character; ++character)
        assert(ssv_overlay_glyph_is_supported(*character));

    const auto regular_7 = render_label("regular", 7);
    const auto regular_14 = render_label("regular", 14);
    const auto bold_7 = render_label("bold", 7);
    assert(regular_7.white_pixels > 0);
    assert(regular_14.height > regular_7.height);
    assert(bold_7.white_pixels > regular_7.white_pixels);
}
