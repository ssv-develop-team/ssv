#include "overlay_renderer.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <stdexcept>

namespace {

void paint_pixel(
    std::uint8_t *data,
    int stride,
    int width,
    int height,
    int x,
    int y,
    int pixel_stride,
    int red_index,
    int green_index,
    int blue_index,
    std::uint8_t red,
    std::uint8_t green,
    std::uint8_t blue)
{
    if (x < 0 || y < 0 || x >= width || y >= height)
        return;
    auto *pixel = data + y * stride + x * pixel_stride;
    pixel[red_index] = red;
    pixel[green_index] = green;
    pixel[blue_index] = blue;
}

void paint_rect(
    std::uint8_t *data,
    int stride,
    int width,
    int height,
    int x1,
    int y1,
    int x2,
    int y2,
    int pixel_stride,
    int red_index,
    int green_index,
    int blue_index)
{
    x1 = std::clamp(x1, 0, width - 1);
    x2 = std::clamp(x2, 0, width - 1);
    y1 = std::clamp(y1, 0, height - 1);
    y2 = std::clamp(y2, 0, height - 1);
    if (x2 <= x1 || y2 <= y1)
        return;

    for (int thickness = 0; thickness < 3; ++thickness) {
        for (int x = x1; x <= x2; ++x) {
            paint_pixel(data, stride, width, height, x, y1 + thickness,
                pixel_stride, red_index, green_index, blue_index, 0, 255, 0);
            paint_pixel(data, stride, width, height, x, y2 - thickness,
                pixel_stride, red_index, green_index, blue_index, 0, 255, 0);
        }
        for (int y = y1; y <= y2; ++y) {
            paint_pixel(data, stride, width, height, x1 + thickness, y,
                pixel_stride, red_index, green_index, blue_index, 0, 255, 0);
            paint_pixel(data, stride, width, height, x2 - thickness, y,
                pixel_stride, red_index, green_index, blue_index, 0, 255, 0);
        }
    }
}

constexpr std::uint8_t kFont5x7[][7] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
    {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E},
    {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10},
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},
    {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E},
    {0x00, 0x04, 0x00, 0x00, 0x04, 0x04, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C},
    {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x0E, 0x11, 0x01, 0x0F, 0x11, 0x11, 0x0F},
    {0x10, 0x10, 0x16, 0x19, 0x11, 0x19, 0x16},
    {0x00, 0x00, 0x0E, 0x10, 0x10, 0x10, 0x0E},
    {0x01, 0x01, 0x0D, 0x13, 0x11, 0x13, 0x0D},
    {0x00, 0x00, 0x0E, 0x11, 0x1F, 0x10, 0x0E},
    {0x06, 0x08, 0x08, 0x1E, 0x08, 0x08, 0x08},
    {0x00, 0x0F, 0x11, 0x11, 0x0F, 0x01, 0x0E},
    {0x10, 0x10, 0x16, 0x19, 0x11, 0x11, 0x11},
    {0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E},
    {0x02, 0x00, 0x06, 0x02, 0x02, 0x12, 0x0C},
    {0x10, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11},
    {0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E},
    {0x00, 0x00, 0x1A, 0x15, 0x15, 0x15, 0x15},
    {0x00, 0x00, 0x16, 0x19, 0x11, 0x11, 0x11},
    {0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E},
    {0x00, 0x00, 0x1E, 0x11, 0x1E, 0x10, 0x10},
    {0x00, 0x00, 0x0D, 0x13, 0x0F, 0x01, 0x01},
    {0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10},
    {0x00, 0x00, 0x0F, 0x10, 0x0E, 0x01, 0x1E},
    {0x08, 0x08, 0x1E, 0x08, 0x08, 0x09, 0x06},
    {0x00, 0x00, 0x11, 0x11, 0x11, 0x13, 0x0D},
    {0x00, 0x00, 0x11, 0x11, 0x11, 0x0A, 0x04},
    {0x00, 0x00, 0x11, 0x15, 0x15, 0x15, 0x0A},
    {0x00, 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11},
    {0x00, 0x00, 0x11, 0x11, 0x0F, 0x01, 0x0E},
    {0x00, 0x00, 0x1F, 0x02, 0x04, 0x08, 0x1F},
    {0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A},
    {0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E},
    {0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E},
};

int glyph_index_for(char character)
{
    if (character >= '0' && character <= '9') return character - '0';
    if (character == 'C') return 10;
    if (character == 'P') return 11;
    if (character == 'F') return 12;
    if (character == 'S') return 13;
    if (character == ':') return 14;
    if (character == '.') return 15;
    if (character == '-') return 16;
    if (character == ' ') return 17;
    if (character >= 'a' && character <= 'z') return 18 + (character - 'a');
    if (character >= 'A' && character <= 'Z')
        return glyph_index_for(static_cast<char>(character - 'A' + 'a'));
    if (character == '#') return 44;
    if (character == '[') return 45;
    if (character == ']') return 46;
    return -1;
}

const std::uint8_t *glyph_for(char character)
{
    const int index = glyph_index_for(character);
    return index >= 0 ? kFont5x7[index] : kFont5x7[17];
}

void paint_text(
    std::uint8_t *data,
    int stride,
    int width,
    int height,
    int x,
    int y,
    const char *text,
    int pixel_stride,
    int red_index,
    int green_index,
    int blue_index,
    std::uint32_t font_size,
    bool bold)
{
    const int glyph_width = std::max(
        1, static_cast<int>((font_size * 5 + 6) / 7));
    const int spacing = std::max(
        1, static_cast<int>((font_size + 6) / 7));
    const int bold_offset = bold
        ? std::max(1, static_cast<int>((font_size + 13) / 14))
        : 0;
    int cursor = x;
    for (const char *character = text; *character; ++character) {
        const auto *glyph = glyph_for(*character);
        for (std::uint32_t row = 0; row < font_size; ++row) {
            const int source_row = static_cast<int>(row * 7 / font_size);
            for (int column = 0; column < glyph_width; ++column) {
                const int source_column = column * 5 / glyph_width;
                if (glyph[source_row] & (1 << (4 - source_column))) {
                    paint_pixel(data, stride, width, height,
                        cursor + column, y + static_cast<int>(row), pixel_stride,
                        red_index, green_index, blue_index, 255, 255, 255);
                    for (int offset = 1; offset <= bold_offset; ++offset) {
                        paint_pixel(data, stride, width, height,
                            cursor + column + offset,
                            y + static_cast<int>(row), pixel_stride,
                            red_index, green_index, blue_index, 255, 255, 255);
                        paint_pixel(data, stride, width, height,
                            cursor + column,
                            y + static_cast<int>(row) + offset, pixel_stride,
                            red_index, green_index, blue_index, 255, 255, 255);
                    }
                }
            }
        }
        cursor += glyph_width + spacing + bold_offset;
    }
}

bool format_layout(
    GstVideoFormat format,
    int &pixel_stride,
    int &red_index,
    int &green_index,
    int &blue_index)
{
    switch (format) {
    case GST_VIDEO_FORMAT_BGR:
        pixel_stride = 3; blue_index = 0; green_index = 1; red_index = 2;
        return true;
    case GST_VIDEO_FORMAT_RGB:
        pixel_stride = 3; red_index = 0; green_index = 1; blue_index = 2;
        return true;
    case GST_VIDEO_FORMAT_BGRx:
    case GST_VIDEO_FORMAT_BGRA:
        pixel_stride = 4; blue_index = 0; green_index = 1; red_index = 2;
        return true;
    case GST_VIDEO_FORMAT_RGBx:
    case GST_VIDEO_FORMAT_RGBA:
        pixel_stride = 4; red_index = 0; green_index = 1; blue_index = 2;
        return true;
    default:
        return false;
    }
}

} // namespace

bool ssv_overlay_glyph_is_supported(char character)
{
    const int index = glyph_index_for(character);
    if (index < 0)
        return false;
    for (int row = 0; row < 7; ++row) {
        if (kFont5x7[index][row] != 0)
            return true;
    }
    return character == ' ';
}

OverlayRenderer::OverlayRenderer(
    std::string_view font_face,
    std::uint32_t font_size)
    : font_size_(font_size)
{
    if (font_face == "regular")
        bold_font_ = false;
    else if (font_face == "bold")
        bold_font_ = true;
    else
        throw std::invalid_argument("font_face must be regular or bold");
    if (font_size < 7 || font_size > 64)
        throw std::invalid_argument("font_size must be in range 7..64");
}

OverlayRenderStats OverlayRenderer::render(
    GstVideoFrame &video_frame,
    const SsvOverlayFrame &overlay_frame) const
{
    OverlayRenderStats stats;
    int pixel_stride = 0;
    int red_index = 0;
    int green_index = 0;
    int blue_index = 0;
    if (!format_layout(
            GST_VIDEO_FRAME_FORMAT(&video_frame),
            pixel_stride, red_index, green_index, blue_index)) {
        stats.unsupported_format = true;
        return stats;
    }

    const int width = GST_VIDEO_FRAME_WIDTH(&video_frame);
    const int height = GST_VIDEO_FRAME_HEIGHT(&video_frame);
    const int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&video_frame, 0);
    auto *data = static_cast<std::uint8_t *>(
        GST_VIDEO_FRAME_PLANE_DATA(&video_frame, 0));
    for (const auto &box : overlay_frame.boxes) {
        const int x1 = static_cast<int>(box.detection.x1 * width);
        const int y1 = static_cast<int>(box.detection.y1 * height);
        const int x2 = static_cast<int>(box.detection.x2 * width);
        const int y2 = static_cast<int>(box.detection.y2 * height);
        if (x2 - x1 < 1 || y2 - y1 < 1) {
            ++stats.skipped_small_boxes;
            continue;
        }

        paint_rect(data, stride, width, height, x1, y1, x2, y2,
            pixel_stride, red_index, green_index, blue_index);
        char label[80];
        if (box.track_id >= 0) {
            const char *flag = "";
            if (box.track_state == SSV_TRACK_NEW && !box.occluded)
                flag = "[N]";
            else if (box.track_state == SSV_TRACK_NEW && box.occluded)
                flag = "[No]";
            else if (box.track_state == SSV_TRACK_MATCHED && box.occluded)
                flag = "[O]";
            std::snprintf(label, sizeof(label), "%s #%d%s %.2f",
                box.detection.class_name, box.track_id, flag,
                box.detection.confidence);
        } else {
            std::snprintf(label, sizeof(label), "%s %.2f",
                box.detection.class_name, box.detection.confidence);
        }
        paint_text(
            data, stride, width, height, x1,
            std::max(0, y1 - static_cast<int>(font_size_) - 2),
            label, pixel_stride, red_index, green_index, blue_index,
            font_size_, bold_font_);
        ++stats.drawn_boxes;
    }
    return stats;
}
