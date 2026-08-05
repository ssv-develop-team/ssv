#pragma once

#include "core/botsort/botsort_tracker.hpp"
#include "ssv_meta.hpp"

#include <memory>
#include <optional>
#include <vector>

namespace botsort {
class SsvTrackAdapter {
public:
    explicit SsvTrackAdapter(TrackerConfig config);

    void reset();

    std::vector<SsvTrackedObject> process(
        std::vector<SsvDetection> detections,
        const PreprocessTransform &transform,
        std::optional<SsvRgbaFrameView> rgba_frame = std::nullopt);

private:
    TrackerConfig config_;
    std::optional<PreprocessTransform> active_transform_;
    std::unique_ptr<BoTSortTracker> tracker_;
};
}  // namespace botsort
