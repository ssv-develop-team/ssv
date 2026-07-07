#include "ssv_yolo_parser.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace ssv::infer {

namespace {

float detection_iou(const SsvDetection &a, const SsvDetection &b)
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

void apply_nms(std::vector<SsvDetection> &detections, float iou_threshold, size_t max_detections)
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

int label_to_index(const std::vector<std::string> &labels, const std::string &target_class)
{
    if (target_class.empty())
        return -1;
    for (size_t i = 0; i < labels.size(); ++i) {
        if (labels[i] == target_class)
            return static_cast<int>(i);
    }
    return -2;
}

OutputFormat infer_output_format(const TensorSpec &spec)
{
    if (spec.shape.size() != 3)
        throw std::invalid_argument("YOLO output must be a 3D tensor");
    int64_t dim1 = spec.shape[1];
    int64_t dim2 = spec.shape[2];
    if (dim2 == 6)
        return OutputFormat::YoloNx6;
    if (dim1 < dim2)
        return OutputFormat::YoloV8;
    if (dim2 > 5)
        return OutputFormat::YoloV5;
    throw std::invalid_argument("cannot infer YOLO output format from model shape");
}

int infer_num_classes(OutputFormat format, const TensorSpec &spec, int label_count)
{
    if (spec.shape.size() != 3)
        throw std::invalid_argument("YOLO output must be a 3D tensor");
    if (format == OutputFormat::YoloNx6)
        return label_count;
    if (format == OutputFormat::YoloV8)
        return static_cast<int>(spec.shape[1] - 4);
    if (format == OutputFormat::YoloV5)
        return static_cast<int>(spec.shape[2] - 5);
    throw std::invalid_argument("cannot infer class count for output format");
}

void set_label(SsvDetection &det, int cls, const std::vector<std::string> &labels)
{
    if (cls >= 0 && cls < static_cast<int>(labels.size()))
        std::snprintf(det.class_name, sizeof(det.class_name), "%s", labels[cls].c_str());
    else
        std::snprintf(det.class_name, sizeof(det.class_name), "class_%d", cls);
}

bool finish_detection(SsvDetection &det, int cls, float score,
                      int target_cls_idx, const std::vector<std::string> &labels)
{
    if (target_cls_idx >= 0 && cls != target_cls_idx)
        return false;
    det.x1 = std::clamp(det.x1, 0.0f, 1.0f);
    det.y1 = std::clamp(det.y1, 0.0f, 1.0f);
    det.x2 = std::clamp(det.x2, 0.0f, 1.0f);
    det.y2 = std::clamp(det.y2, 0.0f, 1.0f);
    if (det.x2 <= det.x1 || det.y2 <= det.y1)
        return false;
    det.confidence = score;
    det.class_id = cls;
    set_label(det, cls, labels);
    return true;
}

struct Letterbox {
    float content_w;
    float content_h;
    float pad_x;
    float pad_y;
};

Letterbox letterbox_for(const PreprocessResult &preprocess)
{
    float scale = std::min(static_cast<float>(preprocess.model_width) / preprocess.original_width,
                           static_cast<float>(preprocess.model_height) / preprocess.original_height);
    float content_w = preprocess.original_width * scale;
    float content_h = preprocess.original_height * scale;
    return {
        content_w,
        content_h,
        (preprocess.model_width - content_w) * 0.5f,
        (preprocess.model_height - content_h) * 0.5f,
    };
}

} // namespace

void YoloOutputParser::configure(const InferenceConfig &config,
                                 const ModelMetadata &metadata,
                                 std::vector<std::string> labels)
{
    if (metadata.outputs.empty())
        throw std::invalid_argument("model has no outputs");
    if (labels.empty())
        throw std::invalid_argument("label-map has no labels");

    labels_ = std::move(labels);
    confidence_threshold_ = config.confidence_threshold;
    output_format_ = config.output_format == OutputFormat::Auto
        ? infer_output_format(metadata.outputs[0])
        : config.output_format;
    num_classes_ = infer_num_classes(output_format_, metadata.outputs[0],
                                     static_cast<int>(labels_.size()));

    if (output_format_ != OutputFormat::YoloNx6 &&
        static_cast<int>(labels_.size()) != num_classes_) {
        throw std::invalid_argument(
            "label-map class count mismatch: model=" + std::to_string(num_classes_) +
            " labels=" + std::to_string(labels_.size()));
    }

    target_class_id_ = label_to_index(labels_, config.target_class);
    if (target_class_id_ == -2)
        throw std::invalid_argument("target-class not found in label-map: " + config.target_class);
}

std::vector<SsvDetection> YoloOutputParser::parse(const std::vector<Tensor> &outputs,
                                                  const PreprocessResult &preprocess) const
{
    if (outputs.empty())
        throw std::runtime_error("backend returned no output tensors");
    const Tensor &output = outputs[0];
    if (output.spec.shape.size() != 3)
        throw std::runtime_error("YOLO output must be a 3D tensor");

    const float *data = output.host_data.data();
    Letterbox lb = letterbox_for(preprocess);
    std::vector<SsvDetection> detections;

    if (output_format_ == OutputFormat::YoloNx6) {
        int n_detections = static_cast<int>(output.spec.shape[1]);
        for (int i = 0; i < n_detections; ++i) {
            const float *row = data + i * 6;
            float x1 = row[0];
            float y1 = row[1];
            float x2 = row[2];
            float y2 = row[3];
            float score = row[4];
            int cls = static_cast<int>(std::round(row[5]));

            if (score < confidence_threshold_)
                continue;

            bool pixel_coords = x1 > 1.5f || y1 > 1.5f || x2 > 1.5f || y2 > 1.5f;
            SsvDetection det{};
            if (pixel_coords) {
                det.x1 = (x1 - lb.pad_x) / lb.content_w;
                det.y1 = (y1 - lb.pad_y) / lb.content_h;
                det.x2 = (x2 - lb.pad_x) / lb.content_w;
                det.y2 = (y2 - lb.pad_y) / lb.content_h;
            } else {
                det.x1 = x1;
                det.y1 = y1;
                det.x2 = x2;
                det.y2 = y2;
            }
            if (finish_detection(det, cls, score, target_class_id_, labels_))
                detections.push_back(det);
        }
    } else if (output_format_ == OutputFormat::YoloV8) {
        int n_anchors = static_cast<int>(output.spec.shape[2]);
        for (int i = 0; i < n_anchors; ++i) {
            float cx = data[0 * n_anchors + i];
            float cy = data[1 * n_anchors + i];
            float w = data[2 * n_anchors + i];
            float h = data[3 * n_anchors + i];

            int best_cls = -1;
            float best_score = 0.0f;
            for (int c = 0; c < num_classes_; ++c) {
                float score = data[(4 + c) * n_anchors + i];
                if (score > best_score) {
                    best_score = score;
                    best_cls = c;
                }
            }
            if (best_score < confidence_threshold_)
                continue;

            bool pixel_coords = cx > 1.5f || cy > 1.5f || w > 1.5f || h > 1.5f;
            SsvDetection det{};
            if (pixel_coords) {
                det.x1 = ((cx - w / 2.0f) - lb.pad_x) / lb.content_w;
                det.y1 = ((cy - h / 2.0f) - lb.pad_y) / lb.content_h;
                det.x2 = ((cx + w / 2.0f) - lb.pad_x) / lb.content_w;
                det.y2 = ((cy + h / 2.0f) - lb.pad_y) / lb.content_h;
            } else {
                det.x1 = cx - w / 2.0f;
                det.y1 = cy - h / 2.0f;
                det.x2 = cx + w / 2.0f;
                det.y2 = cy + h / 2.0f;
            }
            if (finish_detection(det, best_cls, best_score, target_class_id_, labels_))
                detections.push_back(det);
        }
    } else if (output_format_ == OutputFormat::YoloV5) {
        int n_anchors = static_cast<int>(output.spec.shape[1]);
        int row_size = static_cast<int>(output.spec.shape[2]);
        for (int i = 0; i < n_anchors; ++i) {
            const float *row = data + i * row_size;
            float obj = row[4];
            int best_cls = -1;
            float best_score = 0.0f;
            for (int c = 0; c < num_classes_; ++c) {
                float score = obj * row[5 + c];
                if (score > best_score) {
                    best_score = score;
                    best_cls = c;
                }
            }
            if (best_score < confidence_threshold_)
                continue;

            float cx = row[0];
            float cy = row[1];
            float w = row[2];
            float h = row[3];
            bool pixel_coords = cx > 1.5f || cy > 1.5f || w > 1.5f || h > 1.5f;
            SsvDetection det{};
            if (pixel_coords) {
                det.x1 = ((cx - w / 2.0f) - lb.pad_x) / lb.content_w;
                det.y1 = ((cy - h / 2.0f) - lb.pad_y) / lb.content_h;
                det.x2 = ((cx + w / 2.0f) - lb.pad_x) / lb.content_w;
                det.y2 = ((cy + h / 2.0f) - lb.pad_y) / lb.content_h;
            } else {
                det.x1 = cx - w / 2.0f;
                det.y1 = cy - h / 2.0f;
                det.x2 = cx + w / 2.0f;
                det.y2 = cy + h / 2.0f;
            }
            if (finish_detection(det, best_cls, best_score, target_class_id_, labels_))
                detections.push_back(det);
        }
    }

    apply_nms(detections, 0.45f, 50);
    return detections;
}

} // namespace ssv::infer
