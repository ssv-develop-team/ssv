#include "plugin/gstssvinfer.hpp"
#include "ssv_inference_service.hpp"
#include "ssv_logging.hpp"
#include "ssv_meta.hpp"

#include <gst/video/video.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

GST_DEBUG_CATEGORY_STATIC(ssv_infer_debug);
#define GST_CAT_DEFAULT ssv_infer_debug

using ssv::infer::SsvInferenceRequest;
using ssv::infer::SsvInferenceSubmissionResult;
using ssv::infer::SsvInferenceSubmissionStatus;

namespace {

struct GstCapsUnref {
    void operator()(GstCaps *caps) const noexcept
    {
        if (caps != nullptr)
            gst_caps_unref(caps);
    }
};

using GstCapsPtr = std::unique_ptr<GstCaps, GstCapsUnref>;

} // namespace

struct _SsvInfer {
    GstBaseTransform parent;

    gchar *source_id;
    SsvSourceContext *source_context;
    SsvInferenceService *inference_service;
    SsvTimelineCursor *timeline;
    std::shared_ptr<SsvSourceMeta> meta_owner;
    SsvSourceMeta *meta;
    PreprocessTransform *active_transform;
    guint64 frame_id;
    gboolean mock_detect;
};

enum {
    PROP_0,
    PROP_SOURCE_ID,
    PROP_SOURCE_CONTEXT,
    PROP_INFERENCE_SERVICE,
    PROP_MOCK_DETECT,
};

G_DEFINE_TYPE(SsvInfer, ssv_infer, GST_TYPE_BASE_TRANSFORM)

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

static const char *ssv_meta_result_name(SsvMetaResult result)
{
    switch (result) {
    case SsvMetaResult::Published: return "published";
    case SsvMetaResult::Consumed: return "consumed";
    case SsvMetaResult::Empty: return "empty";
    case SsvMetaResult::NoPts: return "no-pts";
    case SsvMetaResult::Occupied: return "occupied";
    case SsvMetaResult::WrongSource: return "wrong-source";
    case SsvMetaResult::WrongGeneration: return "wrong-generation";
    case SsvMetaResult::DuplicatePts: return "duplicate-pts";
    case SsvMetaResult::StalePts: return "stale-pts";
    }
    return "unknown";
}

static bool ssv_infer_publish_is_fatal(SsvMetaResult result)
{
    return result == SsvMetaResult::WrongSource
        || result == SsvMetaResult::Occupied;
}

static GstFlowReturn ssv_infer_publish(
    SsvInfer *self,
    SsvDetectionFrame detections)
{
    const auto result = self->meta
        ? self->meta->publish_detection(std::move(detections))
        : SsvMetaResult::WrongGeneration;
    if (!ssv_infer_publish_is_fatal(result))
        return GST_FLOW_OK;
    GST_ELEMENT_ERROR(self, STREAM, FAILED,
        ("inference metadata publish rejected the frame"),
        ("source-id=%s result=%s", self->source_id,
         ssv_meta_result_name(result)));
    return GST_FLOW_ERROR;
}

static SsvDetectionFrame ssv_infer_empty_detections(
    guint64 frame_id,
    const gchar *source_id,
    const SsvFrameTiming &timing)
{
    SsvDetectionFrame detections;
    detections.frame_id = frame_id;
    detections.source_id = source_id;
    detections.timing = timing;
    return detections;
}

static SsvInferenceSubmissionResult ssv_infer_submit_buffer(
    SsvInfer *self,
    GstBaseTransform *trans,
    GstBuffer *buffer,
    guint64 frame_id,
    const SsvFrameTiming &timing,
    const PreprocessTransform &transform)
{
    GstVideoInfo video_info;
    gst_video_info_init(&video_info);
    GstCapsPtr caps(gst_pad_get_current_caps(trans->sinkpad));
    if (!caps)
        throw std::runtime_error("ssvinfer sink has no negotiated caps");
    const bool valid_caps =
        gst_video_info_from_caps(&video_info, caps.get());
    if (!valid_caps || GST_VIDEO_INFO_FORMAT(&video_info) != GST_VIDEO_FORMAT_RGBA)
        throw std::runtime_error("ssvinfer requires negotiated RGBA caps");

    auto analysis_frame =
        ssv::infer::ssv_inference_service_create_analysis_frame(
            self->inference_service,
            buffer,
            video_info,
            transform,
            timing);
    SsvInferenceRequest request {
        frame_id,
        self->source_id,
        std::move(analysis_frame),
    };

    return ssv::infer::ssv_inference_service_submit(
        self->inference_service, std::move(request));
}

static gboolean ssv_infer_bind_source(SsvInfer *self)
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

static gboolean ssv_infer_start(GstBaseTransform *trans)
{
    auto *self = SSV_INFER(trans);
    if (self->source_id == nullptr || self->source_id[0] == '\0') {
        GST_ELEMENT_ERROR(self, RESOURCE, SETTINGS,
            ("source-id must not be empty"), (nullptr));
        return FALSE;
    }

    if (!ssv_infer_bind_source(self))
        return FALSE;
    delete self->timeline;
    self->timeline = new SsvTimelineCursor(self->meta_owner);
    delete self->active_transform;
    self->active_transform = nullptr;

    if (self->mock_detect) {
        GST_INFO_OBJECT(self, "mock-detect enabled for automated testing");
        return TRUE;
    }
    if (self->inference_service == nullptr
        || !ssv::infer::ssv_inference_service_is_running(
            self->inference_service)) {
        GST_ELEMENT_ERROR(self, RESOURCE, SETTINGS,
            ("runner-owned inference-service is required"),
            ("source-id=%s", self->source_id));
        return FALSE;
    }
    return TRUE;
}

static gboolean ssv_infer_stop(GstBaseTransform *trans)
{
    auto *self = SSV_INFER(trans);
    if (self->inference_service != nullptr && self->source_id != nullptr) {
        ssv::infer::ssv_inference_service_cancel(
            self->inference_service, self->source_id);
    }
    if (self->timeline != nullptr)
        self->timeline->on_lifecycle_reset();
    delete self->timeline;
    self->timeline = nullptr;
    delete self->active_transform;
    self->active_transform = nullptr;
    self->meta = nullptr;
    self->meta_owner.reset();
    return TRUE;
}

static GstFlowReturn ssv_infer_transform_ip(
    GstBaseTransform *trans,
    GstBuffer *buffer)
{
    auto *self = SSV_INFER(trans);
    const auto frame_id = self->frame_id++;
    PreprocessTransform transform;
    try {
        if (!self->mock_detect) {
            transform =
                ssv::infer::ssv_inference_service_preprocess_transform(
                    self->inference_service, self->source_id);
            if (self->active_transform == nullptr
                || *self->active_transform != transform) {
                if (self->timeline != nullptr)
                    self->timeline->on_lifecycle_reset();
                delete self->active_transform;
                self->active_transform = new PreprocessTransform(transform);
            }
        }
    } catch (const std::exception &error) {
        GST_ELEMENT_ERROR(self, STREAM, FAILED,
            ("source geometry is unavailable for inference"),
            ("source-id=%s error=%s", self->source_id, error.what()));
        return GST_FLOW_ERROR;
    }
    const auto update = self->timeline
        ? self->timeline->on_buffer(
              GST_BUFFER_PTS(buffer),
              GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DISCONT))
        : SsvTimelineUpdate {};
    const SsvFrameTiming timing {
        GST_BUFFER_PTS(buffer),
        GST_BUFFER_DURATION(buffer),
        update.generation,
    };

    if (self->mock_detect) {
        auto detections = ssv_infer_empty_detections(
            frame_id, self->source_id, timing);
        SsvDetection detection {};
        std::snprintf(
            detection.class_name, sizeof(detection.class_name), "person");
        detection.confidence = 0.95F;
        detection.x1 = 0.1F;
        detection.y1 = 0.2F;
        detection.x2 = 0.5F;
        detection.y2 = 0.8F;
        detection.class_id = 0;
        detections.detections.push_back(detection);
        return ssv_infer_publish(self, std::move(detections));
    }

    try {
        auto result = ssv_infer_submit_buffer(
            self, trans, buffer, frame_id, timing, transform);
        switch (result.status) {
        case SsvInferenceSubmissionStatus::Completed:
            if (!result.detections.detections.empty()) {
                GST_DEBUG_OBJECT(self,
                    "frame %" G_GUINT64_FORMAT ": %zu detections",
                    result.detections.frame_id,
                    result.detections.detections.size());
            }
            return ssv_infer_publish(
                self, std::move(result.detections));
        case SsvInferenceSubmissionStatus::Failed:
            GST_WARNING_OBJECT(
                self, "inference failed: %s", result.error.c_str());
            return ssv_infer_publish(
                self,
                ssv_infer_empty_detections(
                    frame_id, self->source_id, timing));
        case SsvInferenceSubmissionStatus::Replaced:
        case SsvInferenceSubmissionStatus::Cancelled:
            return GST_FLOW_OK;
        }
    } catch (const std::exception &error) {
        GST_ELEMENT_ERROR(self, STREAM, FAILED,
            ("failed to submit RGBA frame for inference"),
            ("source-id=%s error=%s", self->source_id, error.what()));
    }
    return GST_FLOW_ERROR;
}

static gboolean ssv_infer_sink_event(
    GstBaseTransform *trans,
    GstEvent *event)
{
    auto *self = SSV_INFER(trans);
    if (self->timeline != nullptr) {
        if (GST_EVENT_TYPE(event) == GST_EVENT_SEGMENT) {
            const GstSegment *segment = nullptr;
            gst_event_parse_segment(event, &segment);
            if (segment != nullptr && segment->format == GST_FORMAT_TIME) {
                self->timeline->on_segment({
                    segment->start,
                    segment->time,
                    segment->base,
                    segment->rate,
                });
            }
        } else if (GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_STOP) {
            gboolean reset_time = FALSE;
            gst_event_parse_flush_stop(event, &reset_time);
            self->timeline->on_flush_stop(reset_time);
            if (self->inference_service != nullptr) {
                ssv::infer::ssv_inference_service_cancel(
                    self->inference_service, self->source_id);
            }
        }
    }
    return GST_BASE_TRANSFORM_CLASS(ssv_infer_parent_class)
        ->sink_event(trans, event);
}

static void ssv_infer_set_property(
    GObject *object,
    guint property_id,
    const GValue *value,
    GParamSpec *spec)
{
    auto *self = SSV_INFER(object);
    switch (property_id) {
    case PROP_SOURCE_ID:
        g_free(self->source_id);
        self->source_id = g_value_dup_string(value);
        break;
    case PROP_SOURCE_CONTEXT:
        self->source_context = static_cast<SsvSourceContext *>(
            g_value_get_pointer(value));
        break;
    case PROP_INFERENCE_SERVICE:
        g_set_object(
            &self->inference_service,
            SSV_INFERENCE_SERVICE(g_value_get_object(value)));
        break;
    case PROP_MOCK_DETECT:
        self->mock_detect = g_value_get_boolean(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, spec);
    }
}

static void ssv_infer_get_property(
    GObject *object,
    guint property_id,
    GValue *value,
    GParamSpec *spec)
{
    auto *self = SSV_INFER(object);
    switch (property_id) {
    case PROP_SOURCE_ID:
        g_value_set_string(value, self->source_id);
        break;
    case PROP_SOURCE_CONTEXT:
        g_value_set_pointer(value, self->source_context);
        break;
    case PROP_INFERENCE_SERVICE:
        g_value_set_object(value, self->inference_service);
        break;
    case PROP_MOCK_DETECT:
        g_value_set_boolean(value, self->mock_detect);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, spec);
    }
}

static void ssv_infer_finalize(GObject *object)
{
    auto *self = SSV_INFER(object);
    g_free(self->source_id);
    g_clear_object(&self->inference_service);
    delete self->timeline;
    delete self->active_transform;
    self->meta_owner.reset();
    self->meta_owner.~shared_ptr<SsvSourceMeta>();
    G_OBJECT_CLASS(ssv_infer_parent_class)->finalize(object);
}

static void ssv_infer_class_init(SsvInferClass *klass)
{
    auto *object_class = G_OBJECT_CLASS(klass);
    auto *base_class = GST_BASE_TRANSFORM_CLASS(klass);
    auto *element_class = GST_ELEMENT_CLASS(klass);

    object_class->set_property = ssv_infer_set_property;
    object_class->get_property = ssv_infer_get_property;
    object_class->finalize = ssv_infer_finalize;

    g_object_class_install_property(object_class, PROP_SOURCE_ID,
        g_param_spec_string(
            "source-id",
            "Source ID",
            "Perception metadata source identifier",
            "pipeline-0",
            (GParamFlags)(G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY
                          | G_PARAM_STATIC_STRINGS)));
    g_object_class_install_property(object_class, PROP_SOURCE_CONTEXT,
        g_param_spec_pointer(
            "source-context",
            "Source Context",
            "Borrowed SsvSourceContext owned by the pipeline",
            (GParamFlags)(G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY
                          | G_PARAM_STATIC_STRINGS)));
    g_object_class_install_property(object_class, PROP_INFERENCE_SERVICE,
        g_param_spec_object(
            "inference-service",
            "Inference Service",
            "Runner-owned inference service",
            SSV_TYPE_INFERENCE_SERVICE,
            (GParamFlags)(G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY
                          | G_PARAM_STATIC_STRINGS)));
    g_object_class_install_property(object_class, PROP_MOCK_DETECT,
        g_param_spec_boolean(
            "mock-detect",
            "Mock Detect",
            "Generate fake person detections for automated tests",
            FALSE,
            (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    gst_element_class_set_static_metadata(
        element_class,
        "SSV Inference Adapter",
        "Filter/Effect/Video",
        "Submit model-sized RGBA frames to SsvInferenceService",
        "site-safety-vision");
    gst_element_class_add_static_pad_template(element_class, &sink_template);
    gst_element_class_add_static_pad_template(element_class, &src_template);

    base_class->start = ssv_infer_start;
    base_class->stop = ssv_infer_stop;
    base_class->transform_ip = ssv_infer_transform_ip;
    base_class->sink_event = ssv_infer_sink_event;
    base_class->passthrough_on_same_caps = TRUE;
}

static void ssv_infer_init(SsvInfer *self)
{
    new (&self->meta_owner) std::shared_ptr<SsvSourceMeta>();
    self->source_id = g_strdup("pipeline-0");
    self->source_context = nullptr;
    self->inference_service = nullptr;
    self->timeline = nullptr;
    self->meta = nullptr;
    self->active_transform = nullptr;
    self->frame_id = 0;
    self->mock_detect = FALSE;
}

GST_ELEMENT_REGISTER_DEFINE(
    ssv_infer, "ssvinfer", GST_RANK_NONE, SSV_TYPE_INFER)

static gboolean plugin_init(GstPlugin *plugin)
{
    SSV_GST_DEBUG_INIT(ssv_infer_debug, "ssv-infer");
    return GST_ELEMENT_REGISTER(ssv_infer, plugin);
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    ssvinfer,
    "SSV Inference Adapter Plugin",
    plugin_init,
    "0.1.0",
    "LGPL",
    "site-safety-vision",
    "https://github.com/site-safety-vision/site-safety-vision")
