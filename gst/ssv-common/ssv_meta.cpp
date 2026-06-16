#include "ssv_meta.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

SsvDetectionStore &SsvDetectionStore::instance() {
    static SsvDetectionStore store;
    return store;
}

bool ssv_normalize_detection(SsvDetection &det) {
    if (!std::isfinite(det.confidence) || det.confidence < 0.0f || det.confidence > 1.0f)
        return false;

    if (!std::isfinite(det.x1) || !std::isfinite(det.y1) ||
        !std::isfinite(det.x2) || !std::isfinite(det.y2))
        return false;

    det.x1 = std::clamp(det.x1, 0.0f, 1.0f);
    det.y1 = std::clamp(det.y1, 0.0f, 1.0f);
    det.x2 = std::clamp(det.x2, 0.0f, 1.0f);
    det.y2 = std::clamp(det.y2, 0.0f, 1.0f);

    if (det.x2 <= det.x1 || det.y2 <= det.y1)
        return false;

    if (det.class_id < -1)
        det.class_id = -1;
    if (det.track_id < -1)
        det.track_id = -1;
    if (det.track_state < 0 || det.track_state > 3)
        det.track_state = 0;

    return true;
}

static void normalize_frame_detections(SsvFrameDetections &frame) {
    auto &detections = frame.detections;
    detections.erase(
        std::remove_if(detections.begin(), detections.end(), [](auto &det) {
            return !ssv_normalize_detection(det);
        }),
        detections.end());
}

void SsvDetectionStore::set(SsvFrameDetections det) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (state_ == State::HAS_TRACKS)
        return;  // unpublished data, don't overwrite
    normalize_frame_detections(det);
    current_ = std::move(det);
    overlay_current_.frame_id = current_.frame_id;
    std::snprintf(overlay_current_.source_id, sizeof(overlay_current_.source_id), "%s", current_.source_id);
    overlay_current_.detections = current_.detections;
    state_ = State::HAS_DETECTIONS;
}

SsvFrameDetections SsvDetectionStore::take_for_tracking() {
    std::lock_guard<std::mutex> lock(mtx_);
    if (state_ != State::HAS_DETECTIONS)
        return {};
    state_ = State::EMPTY;
    return std::move(current_);
}

void SsvDetectionStore::set_tracked(SsvFrameDetections det) {
    std::lock_guard<std::mutex> lock(mtx_);
    normalize_frame_detections(det);
    current_ = std::move(det);
    overlay_current_.frame_id = current_.frame_id;
    std::snprintf(overlay_current_.source_id, sizeof(overlay_current_.source_id), "%s", current_.source_id);
    overlay_current_.detections = current_.detections;
    state_ = State::HAS_TRACKS;
}

SsvFrameDetections SsvDetectionStore::take() {
    std::lock_guard<std::mutex> lock(mtx_);
    // Accept both tracked pipeline output and direct detection output for backward compat.
    if (state_ != State::HAS_TRACKS && state_ != State::HAS_DETECTIONS)
        return {};
    state_ = State::EMPTY;
    return std::move(current_);
}

SsvOverlayFrame SsvDetectionStore::peek_latest() {
    std::lock_guard<std::mutex> lock(mtx_);
    return overlay_current_;
}
