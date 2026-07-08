#include "gstssvinfer.hpp"
#include "ssv_inference_engine.hpp"
#include "ssv_logging.hpp"
#include "ssv_meta.hpp"

#include <gst/video/video.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

GST_DEBUG_CATEGORY_STATIC(ssv_infer_debug);
#define GST_CAT_DEFAULT ssv_infer_debug

using ssv::infer::InferenceConfig;
using ssv::infer::InferenceEngine;
using ssv::infer::SsvVideoFrame;

struct _SsvInfer {
    GstBaseTransform parent;

    gchar *runtime;
    gchar *model_path;
    gchar *device;
    gint device_id;
    gchar *precision;
    gchar *model_family;
    gchar *output_format;
    gfloat conf_threshold;
    gchar *target_class;
    gchar *label_map_path;

    InferenceEngine *engine;

    guint64 frame_id;
    gboolean mock_detect;
    gboolean async_infer;

    std::thread *worker;
    std::mutex *worker_mutex;
    std::condition_variable *worker_cv;
    bool worker_stop;
    bool latest_frame_ready;
    SsvVideoFrame *latest_frame;

    std::mutex *fps_mutex;
    guint64 inference_frame_count;
    std::chrono::steady_clock::time_point *inference_fps_started_at;
};

enum {
    PROP_0,
    PROP_RUNTIME,
    PROP_MODEL_PATH,
    PROP_DEVICE,
    PROP_DEVICE_ID,
    PROP_PRECISION,
    PROP_MODEL_FAMILY,
    PROP_OUTPUT_FORMAT,
    PROP_CONF_THRESHOLD,
    PROP_TARGET_CLASS,
    PROP_LABEL_MAP,
    PROP_MOCK_DETECT,
    PROP_ASYNC_INFER,
};

G_DEFINE_TYPE(SsvInfer, ssv_infer, GST_TYPE_BASE_TRANSFORM)

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE(
    "sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS(
        "video/x-raw, format=(string)BGR, "
        "width=(int)[1, MAX], height=(int)[1, MAX], "
        "framerate=(fraction)[0, MAX]"));

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
    "src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS(
        "video/x-raw, format=(string)BGR, "
        "width=(int)[1, MAX], height=(int)[1, MAX], "
        "framerate=(fraction)[0, MAX]"));

static InferenceConfig
ssv_infer_make_config(SsvInfer *self)
{
    InferenceConfig config;
    config.runtime = ssv::infer::parse_runtime_kind(self->runtime ? self->runtime : "auto");
    config.model_path = self->model_path ? self->model_path : "";
    config.device = ssv::infer::parse_device_kind(self->device ? self->device : "auto");
    config.device_id = self->device_id;
    config.precision = ssv::infer::parse_precision_kind(self->precision ? self->precision : "auto");
    config.model_family = ssv::infer::parse_model_family(self->model_family ? self->model_family : "yolo");
    config.output_format = ssv::infer::parse_output_format(self->output_format ? self->output_format : "auto");
    config.confidence_threshold = self->conf_threshold;
    config.target_class = self->target_class ? self->target_class : "";
    config.label_map = self->label_map_path ? self->label_map_path : "";
    return config;
}

static SsvFrameDetections
ssv_infer_empty_detections(const SsvVideoFrame &input)
{
    SsvFrameDetections det;
    det.frame_id = input.frame_id;
    std::snprintf(det.source_id, sizeof(det.source_id), "%s", input.source_id.c_str());
    return det;
}

static void
ssv_infer_note_inference_completed(SsvInfer *self)
{
    if (!self->fps_mutex || !self->inference_fps_started_at)
        return;

    std::lock_guard<std::mutex> lock(*self->fps_mutex);
    self->inference_frame_count++;
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - *self->inference_fps_started_at).count();
    if (elapsed < 1.0)
        return;

    double fps = static_cast<double>(self->inference_frame_count) / elapsed;
    GST_INFO_OBJECT(self, "inference fps: %.1f", fps);
    self->inference_frame_count = 0;
    *self->inference_fps_started_at = now;
}

static void
ssv_infer_run_on_frame(SsvInfer *self, const SsvVideoFrame &input)
{
    SsvFrameDetections det = ssv_infer_empty_detections(input);
    if (!self->engine || !self->engine->loaded()) {
        SsvDetectionStore::instance().set(std::move(det));
        return;
    }

    try {
        det = self->engine->run(input);
    } catch (const std::exception &e) {
        GST_WARNING_OBJECT(self, "inference failed: %s", e.what());
    }
    ssv_infer_note_inference_completed(self);

    if (!det.detections.empty()) {
        GST_DEBUG_OBJECT(self, "frame %" G_GUINT64_FORMAT ": %zu detections",
            det.frame_id, det.detections.size());
    }
    SsvDetectionStore::instance().set(std::move(det));
}

static void
ssv_infer_worker_loop(SsvInfer *self)
{
    while (true) {
        SsvVideoFrame frame;
        {
            std::unique_lock<std::mutex> lock(*self->worker_mutex);
            self->worker_cv->wait(lock, [&] {
                return self->worker_stop || self->latest_frame_ready;
            });
            if (self->worker_stop && !self->latest_frame_ready)
                return;
            frame = std::move(*self->latest_frame);
            self->latest_frame_ready = false;
        }
        ssv_infer_run_on_frame(self, frame);
    }
}

static void
ssv_infer_store_latest_frame(SsvInfer *self, SsvVideoFrame *frame)
{
    std::lock_guard<std::mutex> lock(*self->worker_mutex);
    delete self->latest_frame;
    self->latest_frame = frame;
    self->latest_frame_ready = true;
    self->worker_cv->notify_one();
}

static gboolean
ssv_infer_copy_frame(GstBaseTransform *trans, GstBuffer *buf, guint64 frame_id,
                     SsvVideoFrame *out)
{
    GstVideoFrame frame;
    GstVideoInfo vinfo;
    gst_video_info_init(&vinfo);
    GstCaps *caps = gst_pad_get_current_caps(trans->sinkpad);
    if (!caps || !gst_video_info_from_caps(&vinfo, caps)) {
        if (caps)
            gst_caps_unref(caps);
        return FALSE;
    }
    gst_caps_unref(caps);

    if (!gst_video_frame_map(&frame, &vinfo, buf, GST_MAP_READ))
        return FALSE;

    out->frame_id = frame_id;
    out->source_id = "pipeline-0";
    out->width = GST_VIDEO_FRAME_WIDTH(&frame);
    out->height = GST_VIDEO_FRAME_HEIGHT(&frame);
    out->stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
    const auto *src_data = static_cast<const uint8_t *>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
    out->bgr.assign(src_data, src_data + static_cast<size_t>(out->stride) * out->height);
    gst_video_frame_unmap(&frame);
    return TRUE;
}

static gboolean
ssv_infer_start(GstBaseTransform *trans)
{
    SsvInfer *self = SSV_INFER(trans);

    if (self->mock_detect) {
        GST_INFO_OBJECT(self, "mock-detect enabled, skipping model load");
        return TRUE;
    }

    try {
        InferenceConfig config = ssv_infer_make_config(self);
        self->engine = new InferenceEngine();
        self->engine->start(config);
        auto info = self->engine->backend_info();
        GST_INFO_OBJECT(self, "model loaded: %s runtime=%s active-device=%s provider=%s",
            config.model_path.c_str(),
            ssv::infer::to_string(info.runtime).c_str(),
            ssv::infer::to_string(info.active_device).c_str(),
            info.provider_name.c_str());
    } catch (const std::exception &e) {
        GST_ERROR_OBJECT(self, "model load failed: %s", e.what());
        delete self->engine;
        self->engine = nullptr;
        return FALSE;
    }

    if (self->async_infer) {
        self->worker_mutex = new std::mutex();
        self->worker_cv = new std::condition_variable();
        self->worker_stop = false;
        self->latest_frame_ready = false;
        self->latest_frame = nullptr;
        self->worker = new std::thread(ssv_infer_worker_loop, self);
        GST_INFO_OBJECT(self, "async inference enabled (latest-frame queue)");
    }

    return TRUE;
}

static void
ssv_infer_stop_worker(SsvInfer *self)
{
    if (!self->worker)
        return;

    {
        std::lock_guard<std::mutex> lock(*self->worker_mutex);
        self->worker_stop = true;
    }
    self->worker_cv->notify_one();
    if (self->worker->joinable())
        self->worker->join();
    delete self->worker;
    self->worker = nullptr;
    delete self->latest_frame;
    self->latest_frame = nullptr;
    self->latest_frame_ready = false;
}

static gboolean
ssv_infer_stop(GstBaseTransform *trans)
{
    SsvInfer *self = SSV_INFER(trans);
    ssv_infer_stop_worker(self);
    delete self->engine;
    self->engine = nullptr;
    delete self->worker_cv;
    self->worker_cv = nullptr;
    delete self->worker_mutex;
    self->worker_mutex = nullptr;
    if (self->fps_mutex && self->inference_fps_started_at) {
        std::lock_guard<std::mutex> lock(*self->fps_mutex);
        self->inference_frame_count = 0;
        *self->inference_fps_started_at = std::chrono::steady_clock::now();
    }
    return TRUE;
}

static GstFlowReturn
ssv_infer_transform_ip(GstBaseTransform *trans, GstBuffer *buf)
{
    SsvInfer *self = SSV_INFER(trans);

    SsvFrameDetections det;
    det.frame_id = self->frame_id++;
    std::snprintf(det.source_id, sizeof(det.source_id), "pipeline-0");

    if (self->mock_detect) {
        SsvDetection d{};
        std::snprintf(d.class_name, sizeof(d.class_name), "person");
        d.confidence = 0.95f;
        d.x1 = 0.1f; d.y1 = 0.2f;
        d.x2 = 0.5f; d.y2 = 0.8f;
        d.class_id = 0;
        det.detections.push_back(d);
        SsvDetectionStore::instance().set(std::move(det));
        GST_DEBUG_OBJECT(self, "mock frame %" G_GUINT64_FORMAT, self->frame_id - 1);
        return GST_FLOW_OK;
    }

    auto *frame = new SsvVideoFrame();
    if (!ssv_infer_copy_frame(trans, buf, det.frame_id, frame)) {
        delete frame;
        SsvDetectionStore::instance().set(std::move(det));
        return GST_FLOW_OK;
    }

    if (self->async_infer && self->worker && self->worker_mutex && self->worker_cv) {
        ssv_infer_store_latest_frame(self, frame);
        return GST_FLOW_OK;
    }

    ssv_infer_run_on_frame(self, *frame);
    delete frame;
    return GST_FLOW_OK;
}

static void
ssv_infer_set_property(GObject *object, guint prop_id,
                       const GValue *value, GParamSpec *pspec)
{
    auto *self = SSV_INFER(object);
    switch (prop_id) {
    case PROP_RUNTIME:
        g_free(self->runtime);
        self->runtime = g_value_dup_string(value);
        break;
    case PROP_MODEL_PATH:
        g_free(self->model_path);
        self->model_path = g_value_dup_string(value);
        break;
    case PROP_DEVICE:
        g_free(self->device);
        self->device = g_value_dup_string(value);
        break;
    case PROP_DEVICE_ID:
        self->device_id = g_value_get_int(value);
        break;
    case PROP_PRECISION:
        g_free(self->precision);
        self->precision = g_value_dup_string(value);
        break;
    case PROP_MODEL_FAMILY:
        g_free(self->model_family);
        self->model_family = g_value_dup_string(value);
        break;
    case PROP_OUTPUT_FORMAT:
        g_free(self->output_format);
        self->output_format = g_value_dup_string(value);
        break;
    case PROP_CONF_THRESHOLD:
        self->conf_threshold = g_value_get_float(value);
        break;
    case PROP_TARGET_CLASS:
        g_free(self->target_class);
        self->target_class = g_value_dup_string(value);
        break;
    case PROP_LABEL_MAP:
        g_free(self->label_map_path);
        self->label_map_path = g_value_dup_string(value);
        break;
    case PROP_MOCK_DETECT:
        self->mock_detect = g_value_get_boolean(value);
        break;
    case PROP_ASYNC_INFER:
        self->async_infer = g_value_get_boolean(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void
ssv_infer_get_property(GObject *object, guint prop_id,
                       GValue *value, GParamSpec *pspec)
{
    auto *self = SSV_INFER(object);
    switch (prop_id) {
    case PROP_RUNTIME:
        g_value_set_string(value, self->runtime);
        break;
    case PROP_MODEL_PATH:
        g_value_set_string(value, self->model_path);
        break;
    case PROP_DEVICE:
        g_value_set_string(value, self->device);
        break;
    case PROP_DEVICE_ID:
        g_value_set_int(value, self->device_id);
        break;
    case PROP_PRECISION:
        g_value_set_string(value, self->precision);
        break;
    case PROP_MODEL_FAMILY:
        g_value_set_string(value, self->model_family);
        break;
    case PROP_OUTPUT_FORMAT:
        g_value_set_string(value, self->output_format);
        break;
    case PROP_CONF_THRESHOLD:
        g_value_set_float(value, self->conf_threshold);
        break;
    case PROP_TARGET_CLASS:
        g_value_set_string(value, self->target_class);
        break;
    case PROP_LABEL_MAP:
        g_value_set_string(value, self->label_map_path);
        break;
    case PROP_MOCK_DETECT:
        g_value_set_boolean(value, self->mock_detect);
        break;
    case PROP_ASYNC_INFER:
        g_value_set_boolean(value, self->async_infer);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void
ssv_infer_finalize(GObject *object)
{
    auto *self = SSV_INFER(object);
    g_free(self->runtime);
    g_free(self->model_path);
    g_free(self->device);
    g_free(self->precision);
    g_free(self->model_family);
    g_free(self->output_format);
    g_free(self->target_class);
    g_free(self->label_map_path);
    delete self->engine;
    delete self->latest_frame;
    delete self->worker_cv;
    delete self->worker_mutex;
    delete self->inference_fps_started_at;
    delete self->fps_mutex;
    G_OBJECT_CLASS(ssv_infer_parent_class)->finalize(object);
}

static void
ssv_infer_class_init(SsvInferClass *klass)
{
    auto *gobject_class = G_OBJECT_CLASS(klass);
    auto *base_class = GST_BASE_TRANSFORM_CLASS(klass);
    auto *element_class = GST_ELEMENT_CLASS(klass);

    gobject_class->set_property = ssv_infer_set_property;
    gobject_class->get_property = ssv_infer_get_property;
    gobject_class->finalize = ssv_infer_finalize;

    g_object_class_install_property(gobject_class, PROP_RUNTIME,
        g_param_spec_string("runtime", "Inference Runtime",
            "Inference runtime: auto, onnxruntime, or tensorrt",
            "auto", (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_MODEL_PATH,
        g_param_spec_string("model-path", "Model Path",
            "Path to model file (.onnx or .engine)",
            nullptr, (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_DEVICE,
        g_param_spec_string("device", "Inference Device",
            "Inference device: auto, cpu, or gpu",
            "auto", (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_DEVICE_ID,
        g_param_spec_int("device-id", "Device ID",
            "Device id used when GPU inference is active",
            0, G_MAXINT, 0,
            (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_PRECISION,
        g_param_spec_string("precision", "Precision",
            "Inference precision: auto, fp32, fp16, or int8",
            "auto", (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_MODEL_FAMILY,
        g_param_spec_string("model-family", "Model Family",
            "Model family: auto or yolo",
            "yolo", (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_OUTPUT_FORMAT,
        g_param_spec_string("output-format", "Output Format",
            "Model output format: auto, yolov5, yolov8, or yolo_nx6",
            "auto", (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_CONF_THRESHOLD,
        g_param_spec_float("conf-threshold", "Confidence Threshold",
            "Minimum detection confidence",
            0.0f, 1.0f, 0.5f,
            (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_TARGET_CLASS,
        g_param_spec_string("target-class", "Target Class",
            "Only emit detections for this class (empty = all classes)",
            "", (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_LABEL_MAP,
        g_param_spec_string("label-map", "Label Map",
            "Path to model class label map file",
            nullptr, (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_MOCK_DETECT,
        g_param_spec_boolean("mock-detect", "Mock Detect",
            "Generate fake person detections (no model needed)",
            FALSE, (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_ASYNC_INFER,
        g_param_spec_boolean("async", "Async Inference",
            "Run inference on a background worker using the latest frame only",
            TRUE, (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    gst_element_class_set_static_metadata(element_class,
        "SSV YOLO Inference",
        "Filter/Effect/Video",
        "Run YOLO ONNX inference on video frames",
        "site-safety-vision");

    gst_element_class_add_static_pad_template(element_class, &sink_template);
    gst_element_class_add_static_pad_template(element_class, &src_template);

    base_class->start = ssv_infer_start;
    base_class->stop = ssv_infer_stop;
    base_class->transform_ip = ssv_infer_transform_ip;
    base_class->passthrough_on_same_caps = TRUE;
}

static void
ssv_infer_init(SsvInfer *self)
{
    self->runtime = g_strdup("auto");
    self->model_path = nullptr;
    self->device = g_strdup("auto");
    self->device_id = 0;
    self->precision = g_strdup("auto");
    self->model_family = g_strdup("yolo");
    self->output_format = g_strdup("auto");
    self->conf_threshold = 0.5f;
    self->target_class = g_strdup("");
    self->label_map_path = nullptr;
    self->engine = nullptr;
    self->frame_id = 0;
    self->mock_detect = FALSE;
    self->async_infer = TRUE;
    self->worker = nullptr;
    self->worker_mutex = nullptr;
    self->worker_cv = nullptr;
    self->worker_stop = false;
    self->latest_frame_ready = false;
    self->latest_frame = nullptr;
    self->fps_mutex = new std::mutex();
    self->inference_frame_count = 0;
    self->inference_fps_started_at = new std::chrono::steady_clock::time_point(std::chrono::steady_clock::now());
}

GST_ELEMENT_REGISTER_DEFINE(ssv_infer, "ssvinfer",
    GST_RANK_NONE, SSV_TYPE_INFER)

static gboolean
plugin_init(GstPlugin *plugin)
{
    SSV_GST_DEBUG_INIT(ssv_infer_debug, "ssv-infer");
    return GST_ELEMENT_REGISTER(ssv_infer, plugin);
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR, GST_VERSION_MINOR,
    ssvinfer,
    "SSV YOLO Inference Plugin",
    plugin_init,
    "0.1.0", "LGPL",
    "site-safety-vision",
    "https://github.com/site-safety-vision/site-safety-vision")
