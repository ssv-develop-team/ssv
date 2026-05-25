#include "ssv_meta.hpp"

#include <cstring>
#include <utility>

SsvDetectionStore &SsvDetectionStore::instance() {
    static SsvDetectionStore store;
    return store;
}

void SsvDetectionStore::set(SsvFrameDetections det) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (state_ == State::HAS_TRACKS)
        return;  // unpublished data, don't overwrite
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
    current_ = std::move(det);
    overlay_current_.frame_id = current_.frame_id;
    std::snprintf(overlay_current_.source_id, sizeof(overlay_current_.source_id), "%s", current_.source_id);
    overlay_current_.detections = current_.detections;
    state_ = State::HAS_TRACKS;
}

SsvFrameDetections SsvDetectionStore::take() {
    std::lock_guard<std::mutex> lock(mtx_);
    // Accept both HAS_TRACKS (M3 pipeline with ssvtrack) and
    // HAS_DETECTIONS (M2 pipeline without ssvtrack, backward compat).
    if (state_ != State::HAS_TRACKS && state_ != State::HAS_DETECTIONS)
        return {};
    state_ = State::EMPTY;
    return std::move(current_);
}

SsvOverlayFrame SsvDetectionStore::peek_latest() {
    std::lock_guard<std::mutex> lock(mtx_);
    return overlay_current_;
}
