#include "gstssvinfer.hpp"
#include "ssv_logging.hpp"
#include "ssv_meta.hpp"

#include <gst/video/video.h>
#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

GST_DEBUG_CATEGORY_STATIC(ssv_infer_debug);

// ── COCO 80 class names (standard YOLO training) ──────────────────────
static constexpr const char *COCO_NAMES[] = {
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train",
    "truck", "boat", "traffic light", "fire hydrant", "stop sign",
    "parking meter", "bench", "bird", "cat", "dog", "horse", "sheep",
    "cow", "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella",
    "handbag", "tie", "suitcase", "frisbee", "skis", "snowboard",
    "sports ball", "kite", "baseball bat", "baseball glove", "skateboard",
    "surfboard", "tennis racket", "bottle", "wine glass", "cup", "fork",
    "knife", "spoon", "bowl", "banana", "apple", "sandwich", "orange",
    "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair",
    "couch", "potted plant", "bed", "dining table", "toilet", "tv",
    "laptop", "mouse", "remote", "keyboard", "cell phone", "microwave",
    "oven", "toaster", "sink", "refrigerator", "book", "clock", "vase",
    "scissors", "teddy bear", "hair drier", "toothbrush"
};
static constexpr int NUM_COCO_CLASSES = 80;

struct SsvInferFrame {
    guint64 frame_id = 0;
    std::string source_id;
    int width = 0;
    int height = 0;
    int stride = 0;
    std::vector<uint8_t> bgr;
};

// ── GObject struct (C++ members kept as pointers) ──────────────────────
struct _SsvInfer {
    GstBaseTransform parent;

    gchar *model_path;
    gfloat conf_threshold;
    gchar *target_class;

    Ort::Env *ort_env;
    Ort::Session *ort_session;
    Ort::MemoryInfo *mem_info;

    std::string *input_name;
    int model_h;
    int model_w;
    std::string *output_name;
    int num_classes;
    bool is_yolov8;

    guint64 frame_id;
    gboolean mock_detect;
    gboolean async_infer;

    std::thread *worker;
    std::mutex *worker_mutex;
    std::condition_variable *worker_cv;
    bool worker_stop;
    bool latest_frame_ready;
    SsvInferFrame *latest_frame;
};

enum {
    PROP_0,
    PROP_MODEL_PATH,
    PROP_CONF_THRESHOLD,
    PROP_TARGET_CLASS,
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

// ── Helpers ────────────────────────────────────────────────────────────

static void
preprocess_bgr_to_chw(const uint8_t *src, int src_w, int src_h, int src_stride,
                      float *dst, int dst_w, int dst_h)
{
    float scale = std::min((float)dst_w / src_w, (float)dst_h / src_h);
    int new_w = (int)(src_w * scale);
    int new_h = (int)(src_h * scale);
    int pad_x = (dst_w - new_w) / 2;
    int pad_y = (dst_h - new_h) / 2;

    size_t plane = (size_t)dst_w * dst_h;
    std::fill(dst, dst + 3 * plane, 114.0f / 255.0f);

    float *dst_r = dst;
    float *dst_g = dst + plane;
    float *dst_b = dst + 2 * plane;

    for (int y = 0; y < new_h; ++y) {
        int sy = (int)((y) / scale);
        if (sy >= src_h) sy = src_h - 1;
        const uint8_t *src_row = src + sy * src_stride;
        int dy = y + pad_y;
        for (int x = 0; x < new_w; ++x) {
            int sx = (int)((x) / scale);
            if (sx >= src_w) sx = src_w - 1;
            float b = src_row[sx * 3 + 0] / 255.0f;
            float g = src_row[sx * 3 + 1] / 255.0f;
            float r = src_row[sx * 3 + 2] / 255.0f;
            int di = dy * dst_w + (x + pad_x);
            dst_r[di] = r;
            dst_g[di] = g;
            dst_b[di] = b;
        }
    }
}

static int
target_class_to_index(const char *target_class) {
    if (!target_class || target_class[0] == '\0')
        return -1;
    for (int i = 0; i < NUM_COCO_CLASSES; ++i) {
        if (std::strcmp(target_class, COCO_NAMES[i]) == 0)
            return i;
    }
    return -1;
}

static float
detection_iou(const SsvDetection &a, const SsvDetection &b)
{
    float ix1 = std::max(a.x1, b.x1);
    float iy1 = std::max(a.y1, b.y1);
    float ix2 = std::min(a.x2, b.x2);
    float iy2 = std::min(a.y2, b.y2);
    float iw = std::max(0.0f, ix2 - ix1);
    float ih = std::max(0.0f, iy2 - iy1);
    float inter = iw * ih;
    float area_a = std::max(0.0f, a.x2 - a.x1) * std::max(0.0f, a.y2 - a.y1);
    float area_b = std::max(0.0f, b.x2 - b.x1) * std::max(0.0f, b.y2 - b.y1);
    float uni = area_a + area_b - inter;
    return uni > 0.0f ? inter / uni : 0.0f;
}

static void
apply_nms(std::vector<SsvDetection> &detections, float iou_threshold, size_t max_detections)
{
    std::sort(detections.begin(), detections.end(), [](const auto &a, const auto &b) {
        return a.confidence > b.confidence;
    });

    std::vector<SsvDetection> kept;
    kept.reserve(std::min(detections.size(), max_detections));
    for (const auto &candidate : detections) {
        bool suppressed = false;
        for (const auto &selected : kept) {
            if (candidate.class_id == selected.class_id &&
                detection_iou(candidate, selected) > iou_threshold) {
                suppressed = true;
                break;
            }
        }
        if (!suppressed) {
            kept.push_back(candidate);
            if (kept.size() >= max_detections)
                break;
        }
    }
    detections = std::move(kept);
}

static void
parse_yolov8(const float *data, int n_anchors, int num_classes,
             int input_w, int input_h, int source_w, int source_h,
             float conf_thr, int target_cls_idx,
             SsvFrameDetections *out)
{
    float letterbox_scale = std::min((float)input_w / source_w, (float)input_h / source_h);
    float content_w = source_w * letterbox_scale;
    float content_h = source_h * letterbox_scale;
    float pad_x = (input_w - content_w) * 0.5f;
    float pad_y = (input_h - content_h) * 0.5f;

    for (int i = 0; i < n_anchors; ++i) {
        float cx = data[0 * n_anchors + i];
        float cy = data[1 * n_anchors + i];
        float w  = data[2 * n_anchors + i];
        float h  = data[3 * n_anchors + i];

        int best_cls = -1;
        float best_score = 0.0f;
        for (int c = 0; c < num_classes; ++c) {
            float score = data[(4 + c) * n_anchors + i];
            if (score > best_score) {
                best_score = score;
                best_cls = c;
            }
        }

        if (best_score < conf_thr)
            continue;
        if (target_cls_idx >= 0 && best_cls != target_cls_idx)
            continue;

        bool pixel_coords = cx > 1.5f || cy > 1.5f || w > 1.5f || h > 1.5f;

        SsvDetection det{};
        if (pixel_coords) {
            det.x1 = ((cx - w / 2.0f) - pad_x) / content_w;
            det.y1 = ((cy - h / 2.0f) - pad_y) / content_h;
            det.x2 = ((cx + w / 2.0f) - pad_x) / content_w;
            det.y2 = ((cy + h / 2.0f) - pad_y) / content_h;
        } else {
            det.x1 = cx - w / 2.0f;
            det.y1 = cy - h / 2.0f;
            det.x2 = cx + w / 2.0f;
            det.y2 = cy + h / 2.0f;
        }
        det.x1 = std::clamp(det.x1, 0.0f, 1.0f);
        det.y1 = std::clamp(det.y1, 0.0f, 1.0f);
        det.x2 = std::clamp(det.x2, 0.0f, 1.0f);
        det.y2 = std::clamp(det.y2, 0.0f, 1.0f);
        if (det.x2 <= det.x1 || det.y2 <= det.y1)
            continue;
        det.confidence = best_score;
        det.class_id = best_cls;
        if (best_cls >= 0 && best_cls < NUM_COCO_CLASSES)
            std::snprintf(det.class_name, sizeof(det.class_name), "%s", COCO_NAMES[best_cls]);
        else
            std::snprintf(det.class_name, sizeof(det.class_name), "class_%d", best_cls);
        out->detections.push_back(det);
    }

    apply_nms(out->detections, 0.45f, 50);
}

static void
ssv_infer_run_on_frame(SsvInfer *self, const SsvInferFrame &input) {
    SsvFrameDetections det;
    det.frame_id = input.frame_id;
    std::snprintf(det.source_id, sizeof(det.source_id), "%s", input.source_id.c_str());

    if (!self->ort_session) {
        SsvDetectionStore::instance().set(std::move(det));
        return;
    }

    size_t tensor_size = 3 * (size_t)self->model_w * self->model_h;
    std::vector<float> input_tensor(tensor_size);
    preprocess_bgr_to_chw(input.bgr.data(), input.width, input.height, input.stride,
                          input_tensor.data(), self->model_w, self->model_h);

    std::array<int64_t, 4> input_shape = {1, 3, self->model_h, self->model_w};
    auto input_ort = Ort::Value::CreateTensor<float>(
        *self->mem_info, input_tensor.data(), tensor_size,
        input_shape.data(), input_shape.size());

    const char *input_names[] = { self->input_name->c_str() };
    const char *output_names[] = { self->output_name->c_str() };

    std::vector<Ort::Value> output_tensors;
    try {
        output_tensors = self->ort_session->Run(
            Ort::RunOptions{nullptr},
            input_names, &input_ort, 1,
            output_names, 1);
    } catch (const Ort::Exception &e) {
        GST_WARNING_OBJECT(self, "inference failed: %s", e.what());
        SsvDetectionStore::instance().set(std::move(det));
        return;
    }

    const float *out_data = output_tensors[0].GetTensorData<float>();
    auto out_info = output_tensors[0].GetTensorTypeAndShapeInfo();
    auto out_shape = out_info.GetShape();

    if (self->is_yolov8 && out_shape.size() == 3) {
        int n_anchors = (int)out_shape[2];
        int target_cls_idx = target_class_to_index(self->target_class);
        parse_yolov8(out_data, n_anchors, self->num_classes,
                     self->model_w, self->model_h, input.width, input.height,
                     self->conf_threshold, target_cls_idx, &det);
    }

    if (!det.detections.empty()) {
        GST_DEBUG_OBJECT(self, "frame %" G_GUINT64_FORMAT ": %zu detections",
            det.frame_id, det.detections.size());
    }

    SsvDetectionStore::instance().set(std::move(det));
}

static void
ssv_infer_worker_loop(SsvInfer *self) {
    while (true) {
        SsvInferFrame frame;
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
ssv_infer_store_latest_frame(SsvInfer *self, SsvInferFrame *frame) {
    std::lock_guard<std::mutex> lock(*self->worker_mutex);
    delete self->latest_frame;
    self->latest_frame = frame;
    self->latest_frame_ready = true;
    self->worker_cv->notify_one();
}

// ── GObject lifecycle ──────────────────────────────────────────────────

static gboolean
ssv_infer_start(GstBaseTransform *trans) {
    SsvInfer *self = SSV_INFER(trans);

    if (self->mock_detect) {
        GST_INFO_OBJECT(self, "mock-detect enabled, skipping model load");
        return TRUE;
    }

    if (!self->model_path || self->model_path[0] == '\0') {
        GST_ERROR_OBJECT(self, "model-path not set (use mock-detect=true for testing)");
        return FALSE;
    }

    try {
        self->ort_env = new Ort::Env(ORT_LOGGING_LEVEL_WARNING, "ssv-infer");
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(1);
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        self->ort_session = new Ort::Session(*self->ort_env, self->model_path, opts);
        self->mem_info = new Ort::MemoryInfo(
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault));

        Ort::AllocatorWithDefaultOptions alloc;
        auto in_name = self->ort_session->GetInputNameAllocated(0, alloc);
        self->input_name = new std::string(in_name.get());
        auto in_info = self->ort_session->GetInputTypeInfo(0);
        auto in_tensor = in_info.GetTensorTypeAndShapeInfo();
        auto in_shape = in_tensor.GetShape();
        self->model_h = (int)in_shape[2];
        self->model_w = (int)in_shape[3];

        auto out_name = self->ort_session->GetOutputNameAllocated(0, alloc);
        self->output_name = new std::string(out_name.get());
        auto out_info = self->ort_session->GetOutputTypeInfo(0);
        auto out_tensor = out_info.GetTensorTypeAndShapeInfo();
        auto out_shape = out_tensor.GetShape();

        if (out_shape.size() == 3) {
            int dim1 = (int)out_shape[1];
            int dim2 = (int)out_shape[2];
            if (dim1 < dim2) {
                self->is_yolov8 = true;
                self->num_classes = dim1 - 4;
                GST_INFO_OBJECT(self, "YOLOv8 model: %dx%d, %d classes, %d anchors",
                    self->model_w, self->model_h, self->num_classes, dim2);
            } else {
                self->is_yolov8 = false;
                self->num_classes = dim2 - 5;
                GST_INFO_OBJECT(self, "YOLOv5 model: %dx%d, %d classes, %d anchors",
                    self->model_w, self->model_h, self->num_classes, dim1);
            }
        }

        GST_INFO_OBJECT(self, "model loaded: %s (input %dx%d)",
            self->model_path, self->model_w, self->model_h);
    } catch (const Ort::Exception &e) {
        GST_ERROR_OBJECT(self, "ONNX Runtime error: %s", e.what());
        return FALSE;
    } catch (const std::exception &e) {
        GST_ERROR_OBJECT(self, "model load failed: %s", e.what());
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
ssv_infer_stop_worker(SsvInfer *self) {
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
ssv_infer_stop(GstBaseTransform *trans) {
    SsvInfer *self = SSV_INFER(trans);
    ssv_infer_stop_worker(self);
    delete self->input_name;  self->input_name = nullptr;
    delete self->output_name; self->output_name = nullptr;
    delete self->mem_info;    self->mem_info = nullptr;
    delete self->ort_session; self->ort_session = nullptr;
    delete self->ort_env;     self->ort_env = nullptr;
    delete self->worker_cv;   self->worker_cv = nullptr;
    delete self->worker_mutex; self->worker_mutex = nullptr;
    return TRUE;
}

static GstFlowReturn
ssv_infer_transform_ip(GstBaseTransform *trans, GstBuffer *buf) {
    SsvInfer *self = SSV_INFER(trans);

    SsvFrameDetections det;
    det.frame_id = self->frame_id++;
    std::snprintf(det.source_id, sizeof(det.source_id), "pipeline-0");

    // Mock mode: generate a fake "person" detection
    if (self->mock_detect) {
        SsvDetection d{};
        std::snprintf(d.class_name, sizeof(d.class_name), "person");
        d.confidence = 0.95f;
        d.x1 = 0.1f; d.y1 = 0.2f;
        d.x2 = 0.5f; d.y2 = 0.8f;
        d.class_id = 0;  // COCO person
        det.detections.push_back(d);
        SsvDetectionStore::instance().set(std::move(det));
        GST_DEBUG_OBJECT(self, "mock frame %" G_GUINT64_FORMAT, self->frame_id - 1);
        return GST_FLOW_OK;
    }

    if (self->async_infer && self->worker && self->worker_mutex && self->worker_cv) {
        if (!self->ort_session) {
            SsvDetectionStore::instance().set(std::move(det));
            return GST_FLOW_OK;
        }

        GstVideoFrame frame;
        GstVideoInfo vinfo;
        gst_video_info_init(&vinfo);
        GstCaps *caps = gst_pad_get_current_caps(trans->sinkpad);
        if (!caps || !gst_video_info_from_caps(&vinfo, caps)) {
            if (caps) gst_caps_unref(caps);
            SsvDetectionStore::instance().set(std::move(det));
            return GST_FLOW_OK;
        }
        gst_caps_unref(caps);
        if (!gst_video_frame_map(&frame, &vinfo, buf, GST_MAP_READ)) {
            SsvDetectionStore::instance().set(std::move(det));
            return GST_FLOW_OK;
        }

        auto *latest = new SsvInferFrame();
        latest->frame_id = det.frame_id;
        latest->source_id = "pipeline-0";
        latest->width = GST_VIDEO_FRAME_WIDTH(&frame);
        latest->height = GST_VIDEO_FRAME_HEIGHT(&frame);
        latest->stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
        const uint8_t *src_data = (const uint8_t *)GST_VIDEO_FRAME_PLANE_DATA(&frame, 0);
        latest->bgr.assign(src_data, src_data + (size_t)latest->stride * (size_t)latest->height);
        gst_video_frame_unmap(&frame);

        ssv_infer_store_latest_frame(self, latest);
        return GST_FLOW_OK;
    }

    if (!self->ort_session) {
        SsvDetectionStore::instance().set(std::move(det));
        return GST_FLOW_OK;
    }

    // Map video frame using negotiated caps; an empty GstVideoInfo can disagree
    // with decoder-provided video meta and trigger GStreamer format assertions.
    GstVideoFrame frame;
    GstVideoInfo vinfo;
    gst_video_info_init(&vinfo);
    GstCaps *caps = gst_pad_get_current_caps(trans->sinkpad);
    if (!caps || !gst_video_info_from_caps(&vinfo, caps)) {
        if (caps) gst_caps_unref(caps);
        SsvDetectionStore::instance().set(std::move(det));
        return GST_FLOW_OK;
    }
    gst_caps_unref(caps);
    if (!gst_video_frame_map(&frame, &vinfo, buf, GST_MAP_READ)) {
        SsvDetectionStore::instance().set(std::move(det));
        return GST_FLOW_OK;
    }

    int src_w = GST_VIDEO_FRAME_WIDTH(&frame);
    int src_h = GST_VIDEO_FRAME_HEIGHT(&frame);
    int src_stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
    const uint8_t *src_data = (const uint8_t *)GST_VIDEO_FRAME_PLANE_DATA(&frame, 0);

    size_t tensor_size = 3 * (size_t)self->model_w * self->model_h;
    std::vector<float> input_tensor(tensor_size);
    preprocess_bgr_to_chw(src_data, src_w, src_h, src_stride,
                          input_tensor.data(), self->model_w, self->model_h);

    gst_video_frame_unmap(&frame);

    std::array<int64_t, 4> input_shape = {1, 3, self->model_h, self->model_w};
    auto input_ort = Ort::Value::CreateTensor<float>(
        *self->mem_info, input_tensor.data(), tensor_size,
        input_shape.data(), input_shape.size());

    const char *input_names[] = { self->input_name->c_str() };
    const char *output_names[] = { self->output_name->c_str() };

    std::vector<Ort::Value> output_tensors;
    try {
        output_tensors = self->ort_session->Run(
            Ort::RunOptions{nullptr},
            input_names, &input_ort, 1,
            output_names, 1);
    } catch (const Ort::Exception &e) {
        GST_WARNING_OBJECT(self, "inference failed: %s", e.what());
        SsvDetectionStore::instance().set(std::move(det));
        return GST_FLOW_OK;
    }

    const float *out_data = output_tensors[0].GetTensorData<float>();
    auto out_info = output_tensors[0].GetTensorTypeAndShapeInfo();
    auto out_shape = out_info.GetShape();

    if (self->is_yolov8 && out_shape.size() == 3) {
        int n_anchors = (int)out_shape[2];
        int target_cls_idx = target_class_to_index(self->target_class);
        parse_yolov8(out_data, n_anchors, self->num_classes,
                     self->model_w, self->model_h, src_w, src_h,
                     self->conf_threshold, target_cls_idx, &det);
    }

    if (!det.detections.empty()) {
        GST_DEBUG_OBJECT(self, "frame %" G_GUINT64_FORMAT ": %zu detections",
            det.frame_id, det.detections.size());
    }

    SsvDetectionStore::instance().set(std::move(det));
    return GST_FLOW_OK;
}

// ── Properties ─────────────────────────────────────────────────────────

static void
ssv_infer_set_property(GObject *object, guint prop_id,
                        const GValue *value, GParamSpec *pspec) {
    auto *self = SSV_INFER(object);
    switch (prop_id) {
    case PROP_MODEL_PATH:
        g_free(self->model_path);
        self->model_path = g_value_dup_string(value);
        break;
    case PROP_CONF_THRESHOLD:
        self->conf_threshold = g_value_get_float(value);
        break;
    case PROP_TARGET_CLASS:
        g_free(self->target_class);
        self->target_class = g_value_dup_string(value);
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
                        GValue *value, GParamSpec *pspec) {
    auto *self = SSV_INFER(object);
    switch (prop_id) {
    case PROP_MODEL_PATH:
        g_value_set_string(value, self->model_path);
        break;
    case PROP_CONF_THRESHOLD:
        g_value_set_float(value, self->conf_threshold);
        break;
    case PROP_TARGET_CLASS:
        g_value_set_string(value, self->target_class);
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

// ── Class / instance init ──────────────────────────────────────────────

static void
ssv_infer_finalize(GObject *object) {
    auto *self = SSV_INFER(object);
    g_free(self->model_path);
    g_free(self->target_class);
    delete self->input_name;
    delete self->output_name;
    delete self->mem_info;
    delete self->ort_session;
    delete self->ort_env;
    delete self->latest_frame;
    delete self->worker_cv;
    delete self->worker_mutex;
    G_OBJECT_CLASS(ssv_infer_parent_class)->finalize(object);
}

static void
ssv_infer_class_init(SsvInferClass *klass) {
    auto *gobject_class = G_OBJECT_CLASS(klass);
    auto *base_class = GST_BASE_TRANSFORM_CLASS(klass);
    auto *element_class = GST_ELEMENT_CLASS(klass);

    gobject_class->set_property = ssv_infer_set_property;
    gobject_class->get_property = ssv_infer_get_property;
    gobject_class->finalize = ssv_infer_finalize;

    g_object_class_install_property(gobject_class, PROP_MODEL_PATH,
        g_param_spec_string("model-path", "Model Path",
            "Path to YOLO ONNX model file",
            nullptr, (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_CONF_THRESHOLD,
        g_param_spec_float("conf-threshold", "Confidence Threshold",
            "Minimum detection confidence",
            0.0f, 1.0f, 0.5f,
            (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_TARGET_CLASS,
        g_param_spec_string("target-class", "Target Class",
            "Only emit detections for this class (e.g. person)",
            "person", (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

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
ssv_infer_init(SsvInfer *self) {
    self->model_path = nullptr;
    self->conf_threshold = 0.5f;
    self->target_class = g_strdup("person");
    self->ort_env = nullptr;
    self->ort_session = nullptr;
    self->mem_info = nullptr;
    self->input_name = nullptr;
    self->output_name = nullptr;
    self->model_h = 640;
    self->model_w = 640;
    self->num_classes = 80;
    self->is_yolov8 = true;
    self->frame_id = 0;
    self->mock_detect = FALSE;
    self->async_infer = TRUE;
    self->worker = nullptr;
    self->worker_mutex = nullptr;
    self->worker_cv = nullptr;
    self->worker_stop = false;
    self->latest_frame_ready = false;
    self->latest_frame = nullptr;
}

// ── Plugin registration ────────────────────────────────────────────────

GST_ELEMENT_REGISTER_DEFINE(ssv_infer, "ssvinfer",
    GST_RANK_NONE, SSV_TYPE_INFER)

static gboolean
plugin_init(GstPlugin *plugin) {
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
