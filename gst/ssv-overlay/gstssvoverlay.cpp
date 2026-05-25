#include "ssv_meta.hpp"

#include <gst/base/gstbasetransform.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>

GST_DEBUG_CATEGORY_STATIC(ssv_overlay_debug);

typedef struct _SsvOverlay {
    GstBaseTransform parent;
    gboolean enabled;
    gint frame_count;
    double fps;
    std::chrono::steady_clock::time_point *fps_started_at;
} SsvOverlay;

typedef struct _SsvOverlayClass {
    GstBaseTransformClass parent_class;
} SsvOverlayClass;

#define SSV_TYPE_OVERLAY (ssv_overlay_get_type())
#define SSV_OVERLAY(obj) ((SsvOverlay *)(obj))

G_DEFINE_TYPE(SsvOverlay, ssv_overlay, GST_TYPE_BASE_TRANSFORM)

enum {
    PROP_0,
    PROP_ENABLED,
};

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE(
    "sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS(
        "video/x-raw, format=(string){ BGR, RGB, BGRx, BGRA, RGBx, RGBA }, "
        "width=(int)[1, MAX], height=(int)[1, MAX], "
        "framerate=(fraction)[0, MAX]"));

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
    "src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS(
        "video/x-raw, format=(string){ BGR, RGB, BGRx, BGRA, RGBx, RGBA }, "
        "width=(int)[1, MAX], height=(int)[1, MAX], "
        "framerate=(fraction)[0, MAX]"));

static void
paint_pixel(uint8_t *data, int stride, int width, int height, int x, int y,
            int pixel_stride, int red_index, int green_index, int blue_index,
            uint8_t r, uint8_t g, uint8_t b)
{
    if (x < 0 || y < 0 || x >= width || y >= height)
        return;
    uint8_t *p = data + y * stride + x * pixel_stride;
    p[red_index] = r;
    p[green_index] = g;
    p[blue_index] = b;
}

static void
paint_rect(uint8_t *data, int stride, int width, int height,
           int x1, int y1, int x2, int y2,
           int pixel_stride, int red_index, int green_index, int blue_index)
{
    x1 = std::clamp(x1, 0, width - 1);
    x2 = std::clamp(x2, 0, width - 1);
    y1 = std::clamp(y1, 0, height - 1);
    y2 = std::clamp(y2, 0, height - 1);
    if (x2 <= x1 || y2 <= y1)
        return;

    for (int t = 0; t < 3; ++t) {
        for (int x = x1; x <= x2; ++x) {
            paint_pixel(data, stride, width, height, x, y1 + t,
                        pixel_stride, red_index, green_index, blue_index,
                        0, 255, 0);
            paint_pixel(data, stride, width, height, x, y2 - t,
                        pixel_stride, red_index, green_index, blue_index,
                        0, 255, 0);
        }
        for (int y = y1; y <= y2; ++y) {
            paint_pixel(data, stride, width, height, x1 + t, y,
                        pixel_stride, red_index, green_index, blue_index,
                        0, 255, 0);
            paint_pixel(data, stride, width, height, x2 - t, y,
                        pixel_stride, red_index, green_index, blue_index,
                        0, 255, 0);
        }
    }
}

static constexpr uint8_t FONT_5X7[][7] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, // 0
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}, // 1
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, // 2
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}, // 3
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, // 4
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}, // 5
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, // 6
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, // 7
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, // 8
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}, // 9
    {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}, // C
    {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}, // P
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}, // F
    {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}, // S
    {0x00, 0x04, 0x00, 0x00, 0x04, 0x04, 0x00}, // :
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}, // .
    {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}, // -
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // space
    {0x0E, 0x11, 0x01, 0x0F, 0x11, 0x11, 0x0F}, // a
    {0x10, 0x10, 0x16, 0x19, 0x11, 0x19, 0x16}, // b
    {0x00, 0x00, 0x0E, 0x10, 0x10, 0x10, 0x0E}, // c
    {0x01, 0x01, 0x0D, 0x13, 0x11, 0x13, 0x0D}, // d
    {0x00, 0x00, 0x0E, 0x11, 0x1F, 0x10, 0x0E}, // e
    {0x06, 0x08, 0x08, 0x1E, 0x08, 0x08, 0x08}, // f
    {0x00, 0x0F, 0x11, 0x11, 0x0F, 0x01, 0x0E}, // g
    {0x10, 0x10, 0x16, 0x19, 0x11, 0x11, 0x11}, // h
    {0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E}, // i
    {0x02, 0x00, 0x06, 0x02, 0x02, 0x12, 0x0C}, // j
    {0x10, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, // k
    {0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}, // l
    {0x00, 0x00, 0x1A, 0x15, 0x15, 0x15, 0x15}, // m
    {0x00, 0x00, 0x16, 0x19, 0x11, 0x11, 0x11}, // n
    {0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E}, // o
    {0x00, 0x00, 0x1E, 0x11, 0x1E, 0x10, 0x10}, // p
    {0x00, 0x00, 0x0D, 0x13, 0x0F, 0x01, 0x01}, // q
    {0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10}, // r
    {0x00, 0x00, 0x0F, 0x10, 0x0E, 0x01, 0x1E}, // s
    {0x08, 0x08, 0x1E, 0x08, 0x08, 0x09, 0x06}, // t
    {0x00, 0x00, 0x11, 0x11, 0x11, 0x13, 0x0D}, // u
    {0x00, 0x00, 0x11, 0x11, 0x11, 0x0A, 0x04}, // v
    {0x00, 0x00, 0x11, 0x15, 0x15, 0x15, 0x0A}, // w
    {0x00, 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11}, // x
    {0x00, 0x00, 0x11, 0x11, 0x0F, 0x01, 0x0E}, // y
    {0x00, 0x00, 0x1F, 0x02, 0x04, 0x08, 0x1F}, // z
};

static const uint8_t *glyph_for(char c)
{
    if (c >= '0' && c <= '9') return FONT_5X7[c - '0'];
    if (c == 'C') return FONT_5X7[10];
    if (c == 'P') return FONT_5X7[11];
    if (c == 'F') return FONT_5X7[12];
    if (c == 'S') return FONT_5X7[13];
    if (c == ':') return FONT_5X7[14];
    if (c == '.') return FONT_5X7[15];
    if (c == '-') return FONT_5X7[16];
    if (c == ' ') return FONT_5X7[17];
    if (c >= 'a' && c <= 'z') return FONT_5X7[18 + (c - 'a')];
    if (c >= 'A' && c <= 'Z') return glyph_for((char)(c - 'A' + 'a'));
    return FONT_5X7[17];
}

static void
paint_text(uint8_t *data, int stride, int width, int height, int x, int y,
           const char *text, int pixel_stride, int red_index, int green_index, int blue_index,
           uint8_t r, uint8_t g, uint8_t b)
{
    int cursor = x;
    for (const char *p = text; *p; ++p) {
        const uint8_t *glyph = glyph_for(*p);
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if (glyph[row] & (1 << (4 - col))) {
                    paint_pixel(data, stride, width, height, cursor + col, y + row,
                                pixel_stride, red_index, green_index, blue_index,
                                r, g, b);
                }
            }
        }
        cursor += 6;
    }
}

static bool
format_layout(GstVideoFormat format, int *pixel_stride,
              int *red_index, int *green_index, int *blue_index)
{
    switch (format) {
    case GST_VIDEO_FORMAT_BGR:
        *pixel_stride = 3;
        *blue_index = 0;
        *green_index = 1;
        *red_index = 2;
        return true;
    case GST_VIDEO_FORMAT_RGB:
        *pixel_stride = 3;
        *red_index = 0;
        *green_index = 1;
        *blue_index = 2;
        return true;
    case GST_VIDEO_FORMAT_BGRx:
    case GST_VIDEO_FORMAT_BGRA:
        *pixel_stride = 4;
        *blue_index = 0;
        *green_index = 1;
        *red_index = 2;
        return true;
    case GST_VIDEO_FORMAT_RGBx:
    case GST_VIDEO_FORMAT_RGBA:
        *pixel_stride = 4;
        *red_index = 0;
        *green_index = 1;
        *blue_index = 2;
        return true;
    default:
        return false;
    }
}

static GstFlowReturn
ssv_overlay_transform_ip(GstBaseTransform *trans, GstBuffer *buf)
{
    auto *self = SSV_OVERLAY(trans);
    if (!self->enabled)
        return GST_FLOW_OK;

    GstVideoInfo info;
    gst_video_info_init(&info);
    GstCaps *caps = gst_pad_get_current_caps(trans->sinkpad);
    if (!caps || !gst_video_info_from_caps(&info, caps)) {
        if (caps) gst_caps_unref(caps);
        return GST_FLOW_OK;
    }
    gst_caps_unref(caps);

    GstVideoFrame frame;
    if (!gst_video_frame_map(&frame, &info, buf, GST_MAP_READWRITE))
        return GST_FLOW_OK;

    int width = GST_VIDEO_FRAME_WIDTH(&frame);
    int height = GST_VIDEO_FRAME_HEIGHT(&frame);
    int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
    auto *data = static_cast<uint8_t *>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
    int pixel_stride = 0;
    int red_index = 0;
    int green_index = 0;
    int blue_index = 0;
    if (!format_layout(GST_VIDEO_INFO_FORMAT(&info), &pixel_stride,
                       &red_index, &green_index, &blue_index)) {
        gst_video_frame_unmap(&frame);
        return GST_FLOW_OK;
    }

    self->frame_count++;
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - *self->fps_started_at).count();
    if (elapsed >= 1.0) {
        self->fps = self->frame_count / elapsed;
        self->frame_count = 0;
        *self->fps_started_at = now;
    }

    char fps_text[32];
    std::snprintf(fps_text, sizeof(fps_text), "FPS:%.1f", self->fps);
    paint_text(data, stride, width, height, 8, 8, fps_text,
               pixel_stride, red_index, green_index, blue_index,
               255, 255, 255);

    auto latest = SsvDetectionStore::instance().peek_latest();
    for (const auto &det : latest.detections) {
        int x1 = (int)(det.x1 * width);
        int y1 = (int)(det.y1 * height);
        int x2 = (int)(det.x2 * width);
        int y2 = (int)(det.y2 * height);
        paint_rect(data, stride, width, height, x1, y1, x2, y2,
                   pixel_stride, red_index, green_index, blue_index);

        char label[80];
        std::snprintf(label, sizeof(label), "%s %.2f", det.class_name, det.confidence);
        paint_text(data, stride, width, height, x1, std::max(0, y1 - 9), label,
                   pixel_stride, red_index, green_index, blue_index,
                   255, 255, 255);
    }

    gst_video_frame_unmap(&frame);
    return GST_FLOW_OK;
}

static void
ssv_overlay_set_property(GObject *object, guint prop_id,
                         const GValue *value, GParamSpec *pspec)
{
    auto *self = SSV_OVERLAY(object);
    switch (prop_id) {
    case PROP_ENABLED:
        self->enabled = g_value_get_boolean(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void
ssv_overlay_get_property(GObject *object, guint prop_id,
                         GValue *value, GParamSpec *pspec)
{
    auto *self = SSV_OVERLAY(object);
    switch (prop_id) {
    case PROP_ENABLED:
        g_value_set_boolean(value, self->enabled);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void
ssv_overlay_finalize(GObject *object)
{
    auto *self = SSV_OVERLAY(object);
    delete self->fps_started_at;
    self->fps_started_at = nullptr;
    G_OBJECT_CLASS(ssv_overlay_parent_class)->finalize(object);
}

static void
ssv_overlay_class_init(SsvOverlayClass *klass)
{
    auto *gobject_class = G_OBJECT_CLASS(klass);
    auto *base_class = GST_BASE_TRANSFORM_CLASS(klass);
    auto *element_class = GST_ELEMENT_CLASS(klass);

    gobject_class->set_property = ssv_overlay_set_property;
    gobject_class->get_property = ssv_overlay_get_property;
    gobject_class->finalize = ssv_overlay_finalize;

    g_object_class_install_property(gobject_class, PROP_ENABLED,
        g_param_spec_boolean("enabled", "Enabled", "Draw latest detection boxes",
            TRUE, (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    gst_element_class_set_static_metadata(element_class,
        "SSV Detection Overlay", "Filter/Effect/Video",
        "Draw latest detection boxes on video frames", "site-safety-vision");

    gst_element_class_add_static_pad_template(element_class, &sink_template);
    gst_element_class_add_static_pad_template(element_class, &src_template);

    base_class->transform_ip = ssv_overlay_transform_ip;
    base_class->passthrough_on_same_caps = TRUE;
}

static void
ssv_overlay_init(SsvOverlay *self)
{
    self->enabled = TRUE;
    self->frame_count = 0;
    self->fps = 0.0;
    self->fps_started_at = new std::chrono::steady_clock::time_point(std::chrono::steady_clock::now());
}

GST_ELEMENT_REGISTER_DEFINE(ssv_overlay, "ssvoverlay", GST_RANK_NONE, SSV_TYPE_OVERLAY)

static gboolean
plugin_init(GstPlugin *plugin)
{
    GST_DEBUG_CATEGORY_INIT(ssv_overlay_debug, "ssv-overlay", 0, "SSV overlay");
    return GST_ELEMENT_REGISTER(ssv_overlay, plugin);
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR, GST_VERSION_MINOR,
    ssvoverlay,
    "SSV Detection Overlay Plugin",
    plugin_init,
    "0.1.0", "LGPL",
    "site-safety-vision",
    "https://github.com/site-safety-vision/site-safety-vision")
