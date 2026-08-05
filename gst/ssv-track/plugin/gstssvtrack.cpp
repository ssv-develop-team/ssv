#include "gstssvtrack.hpp"
#include "adapters/ssv_track_adapter.hpp"
#include "ssv_logging.hpp"
#include "ssv_meta.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

GST_DEBUG_CATEGORY_STATIC(ssv_track_debug);

// ── GObject struct ─────────────────────────────────────────────────────
struct _SsvTrack {
    GstBaseTransform parent;

    gchar *source_id;
    SsvSourceContext *source_context;
    gint frame_rate;
    gfloat track_thresh;
    gint track_buffer;
    gfloat match_thresh;
    gboolean mock_track;
    gfloat track_low_thresh;
    gfloat track_high_thresh;
    gfloat new_track_thresh;
    gchar *gmc_method;
    gint gmc_downscale;
    botsort::GmcMethod active_gmc_method;

    botsort::SsvTrackAdapter *adapter;
    std::shared_ptr<SsvSourceMeta> meta_owner;
    SsvSourceMeta *meta;
    std::uint64_t active_generation;
    gint mock_next_id;
};

static std::optional<botsort::GmcMethod>
parse_gmc_method(const char *value) noexcept
{
    if (g_strcmp0(value, "none") == 0)
        return botsort::GmcMethod::kNone;
    if (g_strcmp0(value, "sparse-opt-flow") == 0)
        return botsort::GmcMethod::kSparseOptFlow;
    return std::nullopt;
}

static botsort::TrackerConfig
make_botsort_config(const SsvTrack *self) {
    botsort::TrackerConfig config;
    config.frame_rate = self->frame_rate;
    config.track_thresh = self->track_thresh;
    config.track_buffer = self->track_buffer;
    config.match_thresh = self->match_thresh;
    config.track_low_thresh = self->track_low_thresh;
    config.track_high_thresh = self->track_high_thresh;
    config.new_track_thresh = self->new_track_thresh;
    config.enable_score_fuse = true;
    config.enable_class_constraint = false;
    config.gmc_method = self->active_gmc_method;
    config.gmc_downscale = self->gmc_downscale;
    return config;
}

enum {
    PROP_0,
    PROP_SOURCE_ID,
    PROP_SOURCE_CONTEXT,
    PROP_FRAME_RATE,
    PROP_TRACK_THRESH,
    PROP_TRACK_BUFFER,
    PROP_MATCH_THRESH,
    PROP_MOCK_TRACK,
    PROP_TRACK_LOW_THRESH,
    PROP_TRACK_HIGH_THRESH,
    PROP_NEW_TRACK_THRESH,
    PROP_GMC_METHOD,
    PROP_GMC_DOWNSCALE,
};

G_DEFINE_TYPE(SsvTrack, ssv_track, GST_TYPE_BASE_TRANSFORM)

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE(
    "sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS(
        "video/x-raw, format=(string)RGBA, "
        "width=(int)[1, MAX], height=(int)[1, MAX], "
        "framerate=(fraction)[0, MAX]"));

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
    "src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS(
        "video/x-raw, format=(string)RGBA, "
        "width=(int)[1, MAX], height=(int)[1, MAX], "
        "framerate=(fraction)[0, MAX]"));

// ── GstBaseTransform callbacks ────────────────────────────────────────

static void
ssv_track_reset_adapter(SsvTrack *self)
{
    delete self->adapter;
    self->adapter = nullptr;
    self->mock_next_id = 1;
    if (!self->mock_track)
        self->adapter = new botsort::SsvTrackAdapter(make_botsort_config(self));
}

static gboolean
ssv_track_bind_source(SsvTrack *self)
{
    try {
        if (self->source_context != nullptr) {
            if (self->source_context->source_id() != self->source_id) {
                GST_ELEMENT_ERROR(self, RESOURCE, SETTINGS,
                    ("source-context does not match source-id"),
                    ("source-id=%s, context-source=%s",
                        self->source_id,
                        std::string(self->source_context->source_id()).c_str()));
                return FALSE;
            }
            self->meta_owner = self->source_context->meta();
        } else {
            self->meta_owner = ssv_meta(self->source_id);
        }
        self->meta = self->meta_owner.get();
        return self->meta != nullptr;
    } catch (const std::exception &error) {
        GST_ELEMENT_ERROR(self, RESOURCE, SETTINGS,
            ("source metadata context is invalid"), ("%s", error.what()));
        return FALSE;
    }
}

static gboolean
ssv_track_start(GstBaseTransform *trans) {
    auto *self = SSV_TRACK(trans);

    if (!self->source_id || self->source_id[0] == '\0') {
        GST_ELEMENT_ERROR(self, RESOURCE, SETTINGS,
            ("source-id must not be empty"), (nullptr));
        return FALSE;
    }
    const auto parsed_gmc_method = parse_gmc_method(self->gmc_method);
    if (!parsed_gmc_method) {
        GST_ELEMENT_ERROR(self, RESOURCE, SETTINGS,
            ("gmc-method must be one of: sparse-opt-flow, none"),
            ("configured-value=%s", self->gmc_method ? self->gmc_method : "(null)"));
        return FALSE;
    }
    if (!botsort::gmc_method_available(*parsed_gmc_method)) {
        GST_ELEMENT_ERROR(self, RESOURCE, SETTINGS,
            ("configured GMC method is unavailable in this build"),
            ("gmc-method=%s", self->gmc_method));
        return FALSE;
    }
    self->active_gmc_method = *parsed_gmc_method;

    if (!ssv_track_bind_source(self))
        return FALSE;
    self->active_generation = self->meta->generation();
    ssv_track_reset_adapter(self);

    if (self->mock_track) {
        GST_INFO_OBJECT(self, "mock-track enabled (sequential IDs)");
    } else {
        GST_INFO_OBJECT(self, "BoT-SORT tracker started (buffer=%d, match_thresh=%.2f, track_thresh=%.2f)",
            self->track_buffer, self->match_thresh, self->track_thresh);
    }
    return TRUE;
}

static gboolean
ssv_track_stop(GstBaseTransform *trans) {
    auto *self = SSV_TRACK(trans);
    delete self->adapter;
    self->adapter = nullptr;
    self->meta = nullptr;
    self->meta_owner.reset();
    return TRUE;
}

static GstFlowReturn
ssv_track_transform_ip(GstBaseTransform *trans, GstBuffer *buf) {
    auto *self = SSV_TRACK(trans);
    (void)buf;

    if (!self->meta)
        return GST_FLOW_OK;

    const auto generation_before_consume = self->meta->generation();
    if (generation_before_consume != self->active_generation) {
        self->active_generation = generation_before_consume;
        ssv_track_reset_adapter(self);
    }

    auto consumed = self->meta->consume_detection();
    if (consumed.result != SsvMetaResult::Consumed)
        return GST_FLOW_OK;

    auto detections = std::move(*consumed.frame);
    if (detections.source_id != self->source_id) {
        GST_ELEMENT_ERROR(self, STREAM, FAILED,
            ("detection metadata source does not match track source-id"),
            ("metadata-source=%s, source-id=%s",
                detections.source_id.c_str(), self->source_id));
        return GST_FLOW_ERROR;
    }
    const auto current_generation = self->meta->generation();
    if (detections.timing.generation != current_generation) {
        GST_DEBUG_OBJECT(self,
            "dropping detection from stale generation %" G_GUINT64_FORMAT
            " (current=%" G_GUINT64_FORMAT ")",
            detections.timing.generation, current_generation);
        return GST_FLOW_OK;
    }
    if (detections.timing.generation != self->active_generation) {
        self->active_generation = detections.timing.generation;
        ssv_track_reset_adapter(self);
    }
    std::vector<SsvTrackedObject> objects;

    if (self->mock_track) {
        objects.reserve(detections.detections.size());
        for (auto &detection : detections.detections) {
            SsvTrackedObject object;
            object.detection = std::move(detection);
            object.track_id = self->mock_next_id++;
            objects.push_back(std::move(object));
        }
    } else if (self->adapter) {
        if (!detections.analysis_frame) {
            GST_ELEMENT_ERROR(self, STREAM, FAILED,
                ("real tracker requires an analysis frame"),
                ("source-id=%s, frame-id=%" G_GUINT64_FORMAT,
                    self->source_id, detections.frame_id));
            return GST_FLOW_ERROR;
        }

        const auto &analysis_frame = *detections.analysis_frame;
        const SsvRgbaFrameView *rgba_view = nullptr;
        if (self->active_gmc_method != botsort::GmcMethod::kNone)
            rgba_view = &analysis_frame.view();
        try {
            objects = self->adapter->process(
                std::move(detections.detections),
                analysis_frame.transform(),
                rgba_view ? std::optional<SsvRgbaFrameView>(*rgba_view)
                          : std::nullopt);
        } catch (const std::exception &error) {
            GST_ELEMENT_ERROR(self, STREAM, FAILED,
                ("tracker failed to process the analysis frame"),
                ("source-id=%s, reason=%s", self->source_id, error.what()));
            return GST_FLOW_ERROR;
        }

        if (!objects.empty()) {
            GST_DEBUG_OBJECT(self, "frame %" G_GUINT64_FORMAT ": %zu tracked detections",
                detections.frame_id, objects.size());
        }
    }

    const auto result = self->meta->publish_tracked(
        std::move(detections), std::move(objects));
    if (result == SsvMetaResult::WrongSource ||
        result == SsvMetaResult::Occupied) {
        GST_ELEMENT_ERROR(self, STREAM, FAILED,
            ("tracked metadata publish rejected the frame"),
            ("source-id=%s", self->source_id));
        return GST_FLOW_ERROR;
    }
    return GST_FLOW_OK;
}

// ── Properties ─────────────────────────────────────────────────────────

static void
ssv_track_set_property(GObject *object, guint prop_id,
                        const GValue *value, GParamSpec *pspec) {
    auto *self = SSV_TRACK(object);
    switch (prop_id) {
    case PROP_SOURCE_ID:
        g_free(self->source_id);
        self->source_id = g_value_dup_string(value);
        break;
    case PROP_SOURCE_CONTEXT:
        self->source_context = static_cast<SsvSourceContext *>(
            g_value_get_pointer(value));
        break;
    case PROP_FRAME_RATE:
        self->frame_rate = g_value_get_int(value);
        break;
    case PROP_TRACK_THRESH:
        self->track_thresh = g_value_get_float(value);
        break;
    case PROP_TRACK_BUFFER:
        self->track_buffer = g_value_get_int(value);
        break;
    case PROP_MATCH_THRESH:
        self->match_thresh = g_value_get_float(value);
        break;
    case PROP_MOCK_TRACK:
        self->mock_track = g_value_get_boolean(value);
        break;
    case PROP_TRACK_LOW_THRESH:
        self->track_low_thresh = g_value_get_float(value);
        break;
    case PROP_TRACK_HIGH_THRESH:
        self->track_high_thresh = g_value_get_float(value);
        break;
    case PROP_NEW_TRACK_THRESH:
        self->new_track_thresh = g_value_get_float(value);
        break;
    case PROP_GMC_METHOD:
        g_free(self->gmc_method);
        self->gmc_method = g_value_dup_string(value);
        break;
    case PROP_GMC_DOWNSCALE:
        self->gmc_downscale = g_value_get_int(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void
ssv_track_get_property(GObject *object, guint prop_id,
                        GValue *value, GParamSpec *pspec) {
    auto *self = SSV_TRACK(object);
    switch (prop_id) {
    case PROP_SOURCE_ID:
        g_value_set_string(value, self->source_id);
        break;
    case PROP_SOURCE_CONTEXT:
        g_value_set_pointer(value, self->source_context);
        break;
    case PROP_FRAME_RATE:
        g_value_set_int(value, self->frame_rate);
        break;
    case PROP_TRACK_THRESH:
        g_value_set_float(value, self->track_thresh);
        break;
    case PROP_TRACK_BUFFER:
        g_value_set_int(value, self->track_buffer);
        break;
    case PROP_MATCH_THRESH:
        g_value_set_float(value, self->match_thresh);
        break;
    case PROP_MOCK_TRACK:
        g_value_set_boolean(value, self->mock_track);
        break;
    case PROP_TRACK_LOW_THRESH:
        g_value_set_float(value, self->track_low_thresh);
        break;
    case PROP_TRACK_HIGH_THRESH:
        g_value_set_float(value, self->track_high_thresh);
        break;
    case PROP_NEW_TRACK_THRESH:
        g_value_set_float(value, self->new_track_thresh);
        break;
    case PROP_GMC_METHOD:
        g_value_set_string(value, self->gmc_method);
        break;
    case PROP_GMC_DOWNSCALE:
        g_value_set_int(value, self->gmc_downscale);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

// ── Class / instance init ──────────────────────────────────────────────

static void
ssv_track_finalize(GObject *object) {
    auto *self = SSV_TRACK(object);
    delete self->adapter;
    self->adapter = nullptr;
    g_free(self->source_id);
    self->source_id = nullptr;
    g_free(self->gmc_method);
    self->gmc_method = nullptr;
    self->meta_owner.reset();
    self->meta_owner.~shared_ptr<SsvSourceMeta>();
    G_OBJECT_CLASS(ssv_track_parent_class)->finalize(object);
}

static void
ssv_track_class_init(SsvTrackClass *klass) {
    auto *gobject_class = G_OBJECT_CLASS(klass);
    auto *base_class = GST_BASE_TRANSFORM_CLASS(klass);
    auto *element_class = GST_ELEMENT_CLASS(klass);

    gobject_class->set_property = ssv_track_set_property;
    gobject_class->get_property = ssv_track_get_property;
    gobject_class->finalize = ssv_track_finalize;

    g_object_class_install_property(gobject_class, PROP_SOURCE_ID,
        g_param_spec_string("source-id", "Source ID",
            "Perception metadata source identifier",
            "pipeline-0",
            (GParamFlags)(G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY |
                          G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_SOURCE_CONTEXT,
        g_param_spec_pointer("source-context", "Source Context",
            "Borrowed SsvSourceContext owned by the pipeline",
            (GParamFlags)(G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY |
                          G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_FRAME_RATE,
        g_param_spec_int("frame-rate", "Frame Rate",
            "Pipeline frame rate",
            1, 120, 30,
            (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_TRACK_THRESH,
        g_param_spec_float("track-thresh", "Track Threshold",
            "Tracking confidence threshold",
            0.0f, 1.0f, 0.5f,
            (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_TRACK_BUFFER,
        g_param_spec_int("track-buffer", "Track Buffer",
            "Frames to retain lost tracks",
            1, 300, 30,
            (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_MATCH_THRESH,
        g_param_spec_float("match-thresh", "Match Threshold",
            "BoT-SORT matching threshold",
            0.0f, 1.0f, 0.8f,
            (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_MOCK_TRACK,
        g_param_spec_boolean("mock-track", "Mock Track",
            "Assign sequential IDs without real tracking",
            FALSE, (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_TRACK_LOW_THRESH,
        g_param_spec_float("track-low-thresh", "Track Low Threshold",
            "Low-confidence detection threshold used by BoT-SORT",
            0.0f, 1.0f, 0.1f,
            (GParamFlags)(G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_TRACK_HIGH_THRESH,
        g_param_spec_float("track-high-thresh", "Track High Threshold",
            "High-confidence detection threshold used by BoT-SORT",
            0.0f, 1.0f, 0.6f,
            (GParamFlags)(G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_NEW_TRACK_THRESH,
        g_param_spec_float("new-track-thresh", "New Track Threshold",
            "Minimum confidence required to spawn a new BoT-SORT track",
            0.0f, 1.0f, 0.7f,
            (GParamFlags)(G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_GMC_METHOD,
        g_param_spec_string("gmc-method", "GMC Method",
            "Global motion compensation mode",
            "sparse-opt-flow",
            (GParamFlags)(G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_GMC_DOWNSCALE,
        g_param_spec_int("gmc-downscale", "GMC Downscale",
            "Downscale factor used for GMC estimation",
            1, 8, 2,
            (GParamFlags)(G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY | G_PARAM_STATIC_STRINGS)));

    gst_element_class_set_static_metadata(element_class,
        "SSV BoT-SORT Tracker",
        "Filter/Effect/Video",
        "Multi-object tracking using BoT-SORT",
        "site-safety-vision");

    gst_element_class_add_static_pad_template(element_class, &sink_template);
    gst_element_class_add_static_pad_template(element_class, &src_template);

    base_class->start = ssv_track_start;
    base_class->stop = ssv_track_stop;
    base_class->transform_ip = ssv_track_transform_ip;
    base_class->passthrough_on_same_caps = TRUE;
}

static void
ssv_track_init(SsvTrack *self) {
    new (&self->meta_owner) std::shared_ptr<SsvSourceMeta>();
    self->source_id = g_strdup("pipeline-0");
    self->source_context = nullptr;
    self->frame_rate = 30;
    self->track_thresh = 0.5f;
    self->track_buffer = 30;
    self->match_thresh = 0.8f;
    self->mock_track = FALSE;
    self->track_low_thresh = 0.1f;
    self->track_high_thresh = 0.6f;
    self->new_track_thresh = 0.7f;
    self->gmc_method = g_strdup("sparse-opt-flow");
    self->gmc_downscale = 2;
    self->active_gmc_method = botsort::GmcMethod::kSparseOptFlow;
    self->adapter = nullptr;
    self->meta = nullptr;
    self->active_generation = 0;
    self->mock_next_id = 1;
}

// ── Plugin registration ────────────────────────────────────────────────

GST_ELEMENT_REGISTER_DEFINE(ssv_track, "ssvtrack",
    GST_RANK_NONE, SSV_TYPE_TRACK)

static gboolean
plugin_init(GstPlugin *plugin) {
    SSV_GST_DEBUG_INIT(ssv_track_debug, "ssv-track");
    return GST_ELEMENT_REGISTER(ssv_track, plugin);
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR, GST_VERSION_MINOR,
    ssvtrack,
    "SSV BoT-SORT Tracker Plugin",
    plugin_init,
    "0.1.0", "LGPL",
    "site-safety-vision",
    "https://github.com/site-safety-vision/site-safety-vision")
