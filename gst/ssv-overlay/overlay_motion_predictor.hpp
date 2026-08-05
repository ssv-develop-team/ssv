#pragma once

#include "ssv_meta.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct OverlayMotionPredictorStats {
    std::uint64_t timed_out_boxes = 0;
    std::uint64_t clipped_boxes = 0;
    std::uint64_t invalid_boxes = 0;
};

class OverlayMotionPredictor {
public:
    explicit OverlayMotionPredictor(
        bool motion_prediction = true,
        std::uint32_t max_horizon_ms = 300);

    bool observe(const std::shared_ptr<const SsvTrackedFrame> &snapshot);
    SsvOverlayFrame predict(
        std::string_view source_id,
        const SsvFrameTiming &display_timing);
    void predict_into(
        std::string_view source_id,
        const SsvFrameTiming &display_timing,
        SsvOverlayFrame &output);
    OverlayMotionPredictorStats take_stats();
    void reset();

    std::size_t state_count() const { return states_.size(); }

private:
    struct AxisState {
        double position = 0.0;
        double velocity = 0.0;
        double p00 = 0.0001;
        double p01 = 0.0;
        double p10 = 0.0;
        double p11 = 1.0;
    };

    struct TrackState {
        std::array<AxisState, 4> axes;
        SsvDetection detection;
        SsvTrackState track_state = SSV_TRACK_NEW;
        bool occluded = false;
        std::size_t observations = 0;
    };

    struct DisplayEntry {
        int track_id = -1;
        std::optional<SsvOverlayBox> untracked;
    };

    struct DisplayBoxResult {
        std::optional<SsvOverlayBox> box;
        bool clipped = false;
        bool invalid = false;
    };

    static bool valid_bbox(const SsvDetection &detection);
    static std::array<double, 4> measurement(const SsvDetection &detection);
    static void predict_axis(AxisState &axis, double dt_seconds);
    static void update_axis(AxisState &axis, double measurement);
    static bool same_class(
        const SsvDetection &left,
        const SsvDetection &right);
    static DisplayBoxResult display_box(
        SsvOverlayBox box,
        const std::array<double, 4> &state);

    bool motion_prediction_;
    GstClockTime max_horizon_ns_;
    std::string source_id_;
    std::uint64_t generation_ = 0;
    SsvFrameTiming observation_timing_;
    GstClockTime last_observation_pts_ = GST_CLOCK_TIME_NONE;
    std::unordered_map<int, TrackState> states_;
    std::vector<DisplayEntry> display_entries_;
    OverlayMotionPredictorStats pending_stats_;
};
