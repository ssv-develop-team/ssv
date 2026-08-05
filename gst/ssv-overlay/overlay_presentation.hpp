#pragma once

#include "overlay_motion_predictor.hpp"
#include "ssv_meta.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

struct OverlayPresentationStats {
    std::uint64_t display_frames = 0;
    std::uint64_t no_pts_frames = 0;
    std::uint64_t history_hits = 0;
    std::uint64_t history_misses = 0;
    std::uint64_t future_matches = 0;
    std::uint64_t observation_snapshots = 0;
    std::uint64_t observed_boxes = 0;
    std::uint64_t predicted_boxes = 0;
    std::uint64_t timed_out_boxes = 0;
    std::uint64_t clipped_boxes = 0;
    std::uint64_t invalid_boxes = 0;
    std::uint64_t prediction_age_total_ns = 0;
    GstClockTime max_prediction_age_ns = 0;
    std::size_t max_predictor_states = 0;
};

class OverlayPresentationModel {
public:
    OverlayPresentationModel(
        std::shared_ptr<SsvSourceMeta> meta,
        bool motion_prediction,
        std::uint32_t max_horizon_ms);

    [[nodiscard]] SsvOverlayFrame present(
        const SsvFrameTiming &display_timing);
    void present_into(
        const SsvFrameTiming &display_timing,
        SsvOverlayFrame &output);
    void reset();

    [[nodiscard]] OverlayPresentationStats stats() const { return stats_; }
    [[nodiscard]] SsvMetaStats meta_stats() const { return meta_->stats(); }

private:
    std::shared_ptr<SsvSourceMeta> meta_;
    OverlayMotionPredictor predictor_;
    std::shared_ptr<const SsvTrackedFrame> last_observed_;
    std::uint64_t display_generation_ = 0;
    OverlayPresentationStats stats_;
};
