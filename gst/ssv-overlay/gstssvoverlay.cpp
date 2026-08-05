#include "overlay_runtime.hpp"
#include "ssv_logging.hpp"

#include <gst/base/gstbasetransform.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <exception>
#include <memory>
#include <new>
#include <string>

GST_DEBUG_CATEGORY_STATIC(ssv_overlay_debug);

typedef struct _SsvOverlay {
    GstBaseTransform parent;
    gboolean enabled;
    gchar *source_id;
    SsvSourceContext *source_context;
    gboolean motion_prediction;
    guint max_horizon_ms;
    gchar *font_face;
    guint font_size;
    OverlayRuntime *runtime;
    std::shared_ptr<SsvSourceMeta> meta_owner;
} SsvOverlay;

typedef struct _SsvOverlayClass {
    GstBaseTransformClass parent_class;
} SsvOverlayClass;

#define SSV_TYPE_OVERLAY (ssv_overlay_get_type())
#define SSV_OVERLAY(object) ((SsvOverlay *)(object))

G_DEFINE_TYPE(SsvOverlay, ssv_overlay, GST_TYPE_BASE_TRANSFORM)

enum {
    PROP_0,
    PROP_ENABLED,
    PROP_SOURCE_ID,
    PROP_SOURCE_CONTEXT,
    PROP_MOTION_PREDICTION,
    PROP_MAX_HORIZON_MS,
    PROP_FONT_FACE,
    PROP_FONT_SIZE,
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
ssv_overlay_log_summary(SsvOverlay *self, gboolean final_summary)
{
    if (!self->runtime)
        return;
    const auto stats = self->runtime->stats();
    const auto meta = self->runtime->meta_stats();
    if (final_summary) {
        GST_INFO_OBJECT(self,
            "overlay final: frames=%" G_GUINT64_FORMAT
            " no-pts=%" G_GUINT64_FORMAT
            " history-hit=%" G_GUINT64_FORMAT
            " history-miss=%" G_GUINT64_FORMAT
            " observed=%" G_GUINT64_FORMAT
            " predicted=%" G_GUINT64_FORMAT
            " timed-out=%" G_GUINT64_FORMAT
            " clipped=%" G_GUINT64_FORMAT
            " invalid=%" G_GUINT64_FORMAT
            " max-age-ns=%" G_GUINT64_FORMAT
            " max-states=%zu meta-published=%" G_GUINT64_FORMAT
            " meta-rejected=%" G_GUINT64_FORMAT
            " max-history=%zu",
            stats.display_frames, stats.no_pts_frames,
            stats.history_hits, stats.history_misses,
            stats.observed_boxes, stats.predicted_boxes,
            stats.timed_out_boxes, stats.clipped_boxes,
            stats.invalid_boxes,
            stats.max_prediction_age_ns, stats.max_predictor_states,
            meta.published,
            meta.no_pts + meta.occupied + meta.wrong_source +
                meta.wrong_generation + meta.duplicate_pts +
                meta.stale_pts,
            meta.max_history_depth);
    } else {
        GST_DEBUG_OBJECT(self,
            "overlay: frames=%" G_GUINT64_FORMAT
            " no-pts=%" G_GUINT64_FORMAT
            " history-hit=%" G_GUINT64_FORMAT
            " history-miss=%" G_GUINT64_FORMAT
            " predicted=%" G_GUINT64_FORMAT
            " timed-out=%" G_GUINT64_FORMAT
            " clipped=%" G_GUINT64_FORMAT
            " invalid=%" G_GUINT64_FORMAT
            " max-age-ns=%" G_GUINT64_FORMAT
            " max-states=%zu max-history=%zu",
            stats.display_frames, stats.no_pts_frames,
            stats.history_hits, stats.history_misses,
            stats.predicted_boxes, stats.timed_out_boxes,
            stats.clipped_boxes, stats.invalid_boxes,
            stats.max_prediction_age_ns,
            stats.max_predictor_states, meta.max_history_depth);
    }
}

static gboolean
ssv_overlay_start(GstBaseTransform *transform)
{
    auto *self = SSV_OVERLAY(transform);
    if (!self->source_id || self->source_id[0] == '\0') {
        GST_ELEMENT_ERROR(self, RESOURCE, SETTINGS,
            ("source-id must not be empty"), (nullptr));
        return FALSE;
    }
    try {
        if (self->source_context != nullptr
            && self->source_context->source_id() != self->source_id) {
            GST_ELEMENT_ERROR(self, RESOURCE, SETTINGS,
                ("source-context does not match source-id"),
                ("source-id=%s, context-source=%s",
                    self->source_id,
                    std::string(self->source_context->source_id()).c_str()));
            return FALSE;
        }
        self->meta_owner = self->source_context != nullptr
            ? self->source_context->meta()
            : ssv_meta(self->source_id);
        delete self->runtime;
        self->runtime = new OverlayRuntime(
            self->meta_owner,
            self->motion_prediction,
            self->max_horizon_ms,
            self->font_face ? self->font_face : "",
            self->font_size);
    } catch (const std::exception &error) {
        GST_ELEMENT_ERROR(self, RESOURCE, SETTINGS,
            ("overlay runtime configuration is invalid"),
            ("%s", error.what()));
        delete self->runtime;
        self->runtime = nullptr;
        return FALSE;
    }
    return TRUE;
}

static gboolean
ssv_overlay_stop(GstBaseTransform *transform)
{
    auto *self = SSV_OVERLAY(transform);
    if (self->runtime) {
        ssv_overlay_log_summary(self, TRUE);
        self->runtime->stop();
    }
    delete self->runtime;
    self->runtime = nullptr;
    self->meta_owner.reset();
    return TRUE;
}

static gboolean
ssv_overlay_sink_event(GstBaseTransform *transform, GstEvent *event)
{
    auto *self = SSV_OVERLAY(transform);
    if (self->runtime) {
        if (GST_EVENT_TYPE(event) == GST_EVENT_SEGMENT) {
            const GstSegment *segment = nullptr;
            gst_event_parse_segment(event, &segment);
            if (segment && segment->format == GST_FORMAT_TIME) {
                self->runtime->on_segment({
                    segment->start, segment->time, segment->base, segment->rate});
            }
        } else if (GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_STOP) {
            gboolean reset_time = FALSE;
            gst_event_parse_flush_stop(event, &reset_time);
            self->runtime->on_flush_stop(reset_time);
        }
    }
    return GST_BASE_TRANSFORM_CLASS(ssv_overlay_parent_class)
        ->sink_event(transform, event);
}

static GstFlowReturn
ssv_overlay_transform_ip(GstBaseTransform *transform, GstBuffer *buffer)
{
    auto *self = SSV_OVERLAY(transform);
    if (!self->enabled || !self->runtime)
        return GST_FLOW_OK;

    const auto timing = self->runtime->on_buffer(
        GST_BUFFER_PTS(buffer),
        GST_BUFFER_DURATION(buffer),
        GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DISCONT));
    if (timing.pts == GST_CLOCK_TIME_NONE) {
        if (self->runtime->should_log_summary())
            ssv_overlay_log_summary(self, FALSE);
        return GST_FLOW_OK;
    }

    GstVideoInfo info;
    gst_video_info_init(&info);
    GstCaps *caps = gst_pad_get_current_caps(transform->sinkpad);
    if (!caps || !gst_video_info_from_caps(&info, caps)) {
        if (caps)
            gst_caps_unref(caps);
        return GST_FLOW_OK;
    }
    gst_caps_unref(caps);

    GstVideoFrame video_frame;
    if (!gst_video_frame_map(&video_frame, &info, buffer, GST_MAP_READWRITE))
        return GST_FLOW_OK;
    self->runtime->render(video_frame, timing);
    gst_video_frame_unmap(&video_frame);

    if (self->runtime->should_log_summary())
        ssv_overlay_log_summary(self, FALSE);
    return GST_FLOW_OK;
}

static void
ssv_overlay_set_property(
    GObject *object,
    guint property_id,
    const GValue *value,
    GParamSpec *spec)
{
    auto *self = SSV_OVERLAY(object);
    switch (property_id) {
    case PROP_ENABLED:
        self->enabled = g_value_get_boolean(value);
        break;
    case PROP_SOURCE_ID:
        g_free(self->source_id);
        self->source_id = g_value_dup_string(value);
        break;
    case PROP_SOURCE_CONTEXT:
        self->source_context = static_cast<SsvSourceContext *>(
            g_value_get_pointer(value));
        break;
    case PROP_MOTION_PREDICTION:
        self->motion_prediction = g_value_get_boolean(value);
        break;
    case PROP_MAX_HORIZON_MS:
        self->max_horizon_ms = g_value_get_uint(value);
        break;
    case PROP_FONT_FACE:
        g_free(self->font_face);
        self->font_face = g_value_dup_string(value);
        break;
    case PROP_FONT_SIZE:
        self->font_size = g_value_get_uint(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, spec);
    }
}

static void
ssv_overlay_get_property(
    GObject *object,
    guint property_id,
    GValue *value,
    GParamSpec *spec)
{
    auto *self = SSV_OVERLAY(object);
    switch (property_id) {
    case PROP_ENABLED:
        g_value_set_boolean(value, self->enabled);
        break;
    case PROP_SOURCE_ID:
        g_value_set_string(value, self->source_id);
        break;
    case PROP_SOURCE_CONTEXT:
        g_value_set_pointer(value, self->source_context);
        break;
    case PROP_MOTION_PREDICTION:
        g_value_set_boolean(value, self->motion_prediction);
        break;
    case PROP_MAX_HORIZON_MS:
        g_value_set_uint(value, self->max_horizon_ms);
        break;
    case PROP_FONT_FACE:
        g_value_set_string(value, self->font_face);
        break;
    case PROP_FONT_SIZE:
        g_value_set_uint(value, self->font_size);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, spec);
    }
}

static void
ssv_overlay_finalize(GObject *object)
{
    auto *self = SSV_OVERLAY(object);
    delete self->runtime;
    self->runtime = nullptr;
    self->meta_owner.reset();
    self->meta_owner.~shared_ptr<SsvSourceMeta>();
    g_free(self->source_id);
    self->source_id = nullptr;
    g_free(self->font_face);
    self->font_face = nullptr;
    G_OBJECT_CLASS(ssv_overlay_parent_class)->finalize(object);
}

static void
ssv_overlay_class_init(SsvOverlayClass *klass)
{
    auto *object_class = G_OBJECT_CLASS(klass);
    auto *base_class = GST_BASE_TRANSFORM_CLASS(klass);
    auto *element_class = GST_ELEMENT_CLASS(klass);
    object_class->set_property = ssv_overlay_set_property;
    object_class->get_property = ssv_overlay_get_property;
    object_class->finalize = ssv_overlay_finalize;

    g_object_class_install_property(object_class, PROP_ENABLED,
        g_param_spec_boolean("enabled", "Enabled", "Draw overlays",
            TRUE,
            (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
    g_object_class_install_property(object_class, PROP_SOURCE_ID,
        g_param_spec_string("source-id", "Source ID",
            "Perception metadata source identifier",
            "pipeline-0",
            (GParamFlags)(G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY |
                          G_PARAM_STATIC_STRINGS)));
    g_object_class_install_property(object_class, PROP_SOURCE_CONTEXT,
        g_param_spec_pointer("source-context", "Source Context",
            "Borrowed SsvSourceContext owned by the pipeline",
            (GParamFlags)(G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY |
                          G_PARAM_STATIC_STRINGS)));
    g_object_class_install_property(object_class, PROP_MOTION_PREDICTION,
        g_param_spec_boolean("motion-prediction", "Motion Prediction",
            "Extrapolate tracked boxes to the display PTS",
            TRUE,
            (GParamFlags)(G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY |
                          G_PARAM_STATIC_STRINGS)));
    g_object_class_install_property(object_class, PROP_MAX_HORIZON_MS,
        g_param_spec_uint("max-horizon-ms", "Maximum Horizon",
            "Maximum observation age used for overlay display",
            1, 300, 300,
            (GParamFlags)(G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY |
                          G_PARAM_STATIC_STRINGS)));
    g_object_class_install_property(object_class, PROP_FONT_FACE,
        g_param_spec_string("font-face", "Font Face",
            "Built-in overlay font face: regular or bold",
            "regular",
            (GParamFlags)(G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY |
                          G_PARAM_STATIC_STRINGS)));
    g_object_class_install_property(object_class, PROP_FONT_SIZE,
        g_param_spec_uint("font-size", "Font Size",
            "Overlay label height in pixels",
            7, 64, 7,
            (GParamFlags)(G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY |
                          G_PARAM_STATIC_STRINGS)));

    gst_element_class_set_static_metadata(element_class,
        "SSV Detection Overlay", "Filter/Effect/Video",
        "Draw causally matched tracked detections on video frames",
        "site-safety-vision");
    gst_element_class_add_static_pad_template(element_class, &sink_template);
    gst_element_class_add_static_pad_template(element_class, &src_template);

    base_class->start = ssv_overlay_start;
    base_class->stop = ssv_overlay_stop;
    base_class->sink_event = ssv_overlay_sink_event;
    base_class->transform_ip = ssv_overlay_transform_ip;
    base_class->passthrough_on_same_caps = TRUE;
}

static void
ssv_overlay_init(SsvOverlay *self)
{
    new (&self->meta_owner) std::shared_ptr<SsvSourceMeta>();
    self->enabled = TRUE;
    self->source_id = g_strdup("pipeline-0");
    self->source_context = nullptr;
    self->motion_prediction = TRUE;
    self->max_horizon_ms = 300;
    self->font_face = g_strdup("regular");
    self->font_size = 7;
    self->runtime = nullptr;
}

GST_ELEMENT_REGISTER_DEFINE(ssv_overlay, "ssvoverlay",
    GST_RANK_NONE, SSV_TYPE_OVERLAY)

static gboolean
plugin_init(GstPlugin *plugin)
{
    SSV_GST_DEBUG_INIT(ssv_overlay_debug, "ssv-overlay");
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
