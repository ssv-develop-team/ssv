#include "adapters/ssv_track_adapter.hpp"
#include "core/botsort/botsort_coordinates.hpp"

#include <stdexcept>
#include <utility>
namespace botsort {
namespace {
Detection ssv_to_botsort_detection(const SsvDetection &src, int input_index) {
    Detection det;
    det.x1 = src.x1; det.y1 = src.y1; det.x2 = src.x2; det.y2 = src.y2;
    det.score = src.confidence; det.class_id = src.class_id;
    det.class_name = src.class_name; det.input_index = input_index;
    return det;
}
FrameDetections ssv_to_botsort_detections(const std::vector<SsvDetection> &src, int width, int height) {
    FrameDetections out;
    out.reserve(src.size());
    for (std::size_t i = 0; i < src.size(); ++i)
        out.push_back(to_pixel_detection(ssv_to_botsort_detection(src[i], static_cast<int>(i)), width, height));
    return out;
}
std::vector<SsvTrackedObject> make_tracked_objects(
    std::vector<SsvDetection> detections,
    const FrameDetections &results) {
    std::vector<SsvTrackedObject> objects;
    objects.reserve(detections.size());
    for (auto &detection : detections) {
        SsvTrackedObject object;
        object.detection = std::move(detection);
        objects.push_back(std::move(object));
    }
    for (const auto &det : results) {
        if (det.input_index < 0) continue;
        const std::size_t index = static_cast<std::size_t>(det.input_index);
        if (index >= objects.size()) continue;
        auto &out = objects[index];
        out.track_id = det.track_id;
        out.track_state = static_cast<SsvTrackState>(det.track_state);
        out.occluded = det.occluded;
    }
    return objects;
}

bool same_geometry(
    const PreprocessTransform &left,
    const PreprocessTransform &right)
{
    return left.source_width == right.source_width
        && left.source_height == right.source_height
        && left.model_width == right.model_width
        && left.model_height == right.model_height
        && left.scale_x == right.scale_x
        && left.scale_y == right.scale_y
        && left.pad_left == right.pad_left
        && left.pad_top == right.pad_top
        && left.pad_right == right.pad_right
        && left.pad_bottom == right.pad_bottom;
}
}
SsvTrackAdapter::SsvTrackAdapter(TrackerConfig config)
    : config_(config)
    , tracker_(std::make_unique<BoTSortTracker>(config_))
{
}

void SsvTrackAdapter::reset()
{
    active_transform_.reset();
    tracker_ = std::make_unique<BoTSortTracker>(config_);
}

std::vector<SsvTrackedObject> SsvTrackAdapter::process(
    std::vector<SsvDetection> detections,
    const PreprocessTransform &transform,
    std::optional<SsvRgbaFrameView> rgba_frame) {
    if (transform.source_width <= 0 || transform.source_height <= 0)
        throw std::invalid_argument("tracker source dimensions must be positive");
    if (active_transform_ && !same_geometry(*active_transform_, transform))
        reset();
    active_transform_ = transform;

    auto input = ssv_to_botsort_detections(
        detections, transform.source_width, transform.source_height);
    GmcFrameView gmc_frame;
    if (rgba_frame) {
        gmc_frame.rgba = rgba_frame->bytes;
        gmc_frame.model_width = rgba_frame->width;
        gmc_frame.model_height = rgba_frame->height;
        gmc_frame.rgba_stride = rgba_frame->stride;
        gmc_frame.source_to_model_scale_x = transform.scale_x;
        gmc_frame.source_to_model_scale_y = transform.scale_y;
        gmc_frame.pad_left = transform.pad_left;
        gmc_frame.pad_top = transform.pad_top;
    }
    UpdateResult result = tracker_->update(
        input,
        rgba_frame ? std::optional<GmcFrameView>(gmc_frame) : std::nullopt);
    for (auto &tracked : result.detections)
        tracked = to_normalized_detection(
            tracked, transform.source_width, transform.source_height);
    return make_tracked_objects(std::move(detections), result.detections);
}
}  // namespace botsort
