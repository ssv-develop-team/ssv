#include "overlay_motion_predictor.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace {

constexpr double kProcessNoise = 0.0001;
constexpr double kMeasurementNoise = 0.0001;

} // namespace

OverlayMotionPredictor::OverlayMotionPredictor(
    bool motion_prediction,
    std::uint32_t max_horizon_ms)
    : motion_prediction_(motion_prediction),
      max_horizon_ns_(static_cast<GstClockTime>(max_horizon_ms) * GST_MSECOND)
{
    if (max_horizon_ms == 0 || max_horizon_ms > 300)
        throw std::invalid_argument("max_horizon_ms must be in 1..300");
}

bool OverlayMotionPredictor::valid_bbox(const SsvDetection &detection)
{
    return std::isfinite(detection.x1) && std::isfinite(detection.y1) &&
        std::isfinite(detection.x2) && std::isfinite(detection.y2) &&
        std::isfinite(detection.confidence) &&
        detection.x2 > detection.x1 && detection.y2 > detection.y1;
}

std::array<double, 4> OverlayMotionPredictor::measurement(
    const SsvDetection &detection)
{
    return {
        (static_cast<double>(detection.x1) + detection.x2) / 2.0,
        (static_cast<double>(detection.y1) + detection.y2) / 2.0,
        static_cast<double>(detection.x2) - detection.x1,
        static_cast<double>(detection.y2) - detection.y1,
    };
}

void OverlayMotionPredictor::predict_axis(AxisState &axis, double dt_seconds)
{
    axis.position += dt_seconds * axis.velocity;

    const double dt2 = dt_seconds * dt_seconds;
    const double dt3 = dt2 * dt_seconds;
    const double dt4 = dt2 * dt2;
    const double p00 = axis.p00 + dt_seconds * (axis.p01 + axis.p10) +
        dt2 * axis.p11 + kProcessNoise * dt4 / 4.0;
    const double p01 = axis.p01 + dt_seconds * axis.p11 +
        kProcessNoise * dt3 / 2.0;
    const double p10 = axis.p10 + dt_seconds * axis.p11 +
        kProcessNoise * dt3 / 2.0;
    const double p11 = axis.p11 + kProcessNoise * dt2;
    axis.p00 = p00;
    axis.p01 = p01;
    axis.p10 = p10;
    axis.p11 = p11;
}

void OverlayMotionPredictor::update_axis(AxisState &axis, double measured)
{
    const double innovation_variance = axis.p00 + kMeasurementNoise;
    const double position_gain = axis.p00 / innovation_variance;
    const double velocity_gain = axis.p10 / innovation_variance;
    const double residual = measured - axis.position;
    axis.position += position_gain * residual;
    axis.velocity += velocity_gain * residual;

    const double p00 = axis.p00;
    const double p01 = axis.p01;
    axis.p00 = (1.0 - position_gain) * p00;
    axis.p01 = (1.0 - position_gain) * p01;
    axis.p10 -= velocity_gain * p00;
    axis.p11 -= velocity_gain * p01;
    const double cross = (axis.p01 + axis.p10) / 2.0;
    axis.p01 = cross;
    axis.p10 = cross;
}

bool OverlayMotionPredictor::same_class(
    const SsvDetection &left,
    const SsvDetection &right)
{
    return left.class_id == right.class_id &&
        std::strncmp(
            left.class_name, right.class_name, sizeof(left.class_name)) == 0;
}

OverlayMotionPredictor::DisplayBoxResult OverlayMotionPredictor::display_box(
    SsvOverlayBox box,
    const std::array<double, 4> &state)
{
    DisplayBoxResult result;
    const double center_x = state[0];
    const double center_y = state[1];
    const double width = state[2];
    const double height = state[3];
    if (!std::isfinite(center_x) || !std::isfinite(center_y) ||
        !std::isfinite(width) || !std::isfinite(height) ||
        width <= 0.0 || height <= 0.0) {
        result.invalid = true;
        return result;
    }

    const double x1 = center_x - width / 2.0;
    const double y1 = center_y - height / 2.0;
    const double x2 = center_x + width / 2.0;
    const double y2 = center_y + height / 2.0;
    if (x2 <= 0.0 || y2 <= 0.0 || x1 >= 1.0 || y1 >= 1.0)
        return result;

    result.clipped = x1 < 0.0 || y1 < 0.0 || x2 > 1.0 || y2 > 1.0;

    box.detection.x1 = static_cast<float>(std::clamp(x1, 0.0, 1.0));
    box.detection.y1 = static_cast<float>(std::clamp(y1, 0.0, 1.0));
    box.detection.x2 = static_cast<float>(std::clamp(x2, 0.0, 1.0));
    box.detection.y2 = static_cast<float>(std::clamp(y2, 0.0, 1.0));
    if (box.detection.x2 <= box.detection.x1 ||
        box.detection.y2 <= box.detection.y1) {
        result.invalid = true;
        return result;
    }
    result.box = std::move(box);
    return result;
}

bool OverlayMotionPredictor::observe(
    const std::shared_ptr<const SsvTrackedFrame> &snapshot)
{
    if (!snapshot || snapshot->source_id.empty() ||
        snapshot->timing.pts == GST_CLOCK_TIME_NONE) {
        return false;
    }

    const bool new_timeline = source_id_ != snapshot->source_id ||
        generation_ != snapshot->timing.generation;
    if (new_timeline) {
        reset();
        source_id_ = snapshot->source_id;
        generation_ = snapshot->timing.generation;
    } else if (last_observation_pts_ != GST_CLOCK_TIME_NONE &&
               snapshot->timing.pts <= last_observation_pts_) {
        return false;
    } else if (last_observation_pts_ != GST_CLOCK_TIME_NONE &&
               snapshot->timing.pts - last_observation_pts_ >
                   max_horizon_ns_) {
        pending_stats_.timed_out_boxes += display_entries_.size();
        states_.clear();
        display_entries_.clear();
        observation_timing_ = {};
        last_observation_pts_ = GST_CLOCK_TIME_NONE;
    }

    std::unordered_set<int> seen_track_ids;
    std::vector<DisplayEntry> entries;
    entries.reserve(snapshot->objects.size());
    for (const auto &object : snapshot->objects) {
        if (object.track_state == SSV_TRACK_LOST ||
            object.track_state == SSV_TRACK_DEAD) {
            if (object.track_id >= 0)
                states_.erase(object.track_id);
            continue;
        }
        if (!valid_bbox(object.detection)) {
            ++pending_stats_.invalid_boxes;
            if (object.track_id >= 0)
                states_.erase(object.track_id);
            continue;
        }

        if (object.track_id < 0) {
            SsvOverlayBox box;
            box.detection = object.detection;
            box.track_id = object.track_id;
            box.track_state = object.track_state;
            box.occluded = object.occluded;
            entries.push_back({-1, std::move(box)});
            continue;
        }
        if (!seen_track_ids.insert(object.track_id).second)
            continue;

        const auto measured = measurement(object.detection);
        auto state = states_.find(object.track_id);
        if (state == states_.end() ||
            !same_class(state->second.detection, object.detection)) {
            TrackState initialized;
            for (std::size_t axis = 0; axis < initialized.axes.size(); ++axis)
                initialized.axes[axis].position = measured[axis];
            initialized.detection = object.detection;
            initialized.track_state = object.track_state;
            initialized.occluded = object.occluded;
            initialized.observations = 1;
            states_.insert_or_assign(object.track_id, std::move(initialized));
        } else {
            const double dt_seconds = static_cast<double>(
                snapshot->timing.pts - last_observation_pts_) / GST_SECOND;
            for (std::size_t axis = 0; axis < state->second.axes.size(); ++axis) {
                predict_axis(state->second.axes[axis], dt_seconds);
                update_axis(state->second.axes[axis], measured[axis]);
            }
            state->second.detection = object.detection;
            state->second.track_state = object.track_state;
            state->second.occluded = object.occluded;
            ++state->second.observations;
        }
        entries.push_back({object.track_id, std::nullopt});
    }

    for (auto state = states_.begin(); state != states_.end();) {
        if (!seen_track_ids.contains(state->first))
            state = states_.erase(state);
        else
            ++state;
    }

    display_entries_ = std::move(entries);
    source_id_ = snapshot->source_id;
    generation_ = snapshot->timing.generation;
    observation_timing_ = snapshot->timing;
    last_observation_pts_ = snapshot->timing.pts;
    return true;
}

SsvOverlayFrame OverlayMotionPredictor::predict(
    std::string_view source_id,
    const SsvFrameTiming &display_timing)
{
    SsvOverlayFrame output;
    predict_into(source_id, display_timing, output);
    return output;
}

void OverlayMotionPredictor::predict_into(
    std::string_view source_id,
    const SsvFrameTiming &display_timing,
    SsvOverlayFrame &output)
{
    output.source_id = std::string(source_id);
    output.observation_timing = observation_timing_;
    output.display_timing = display_timing;
    output.boxes.clear();
    if (source_id != source_id_ || display_timing.generation != generation_ ||
        display_timing.pts == GST_CLOCK_TIME_NONE ||
        last_observation_pts_ == GST_CLOCK_TIME_NONE ||
        display_timing.pts < last_observation_pts_) {
        return;
    }

    const GstClockTime age = display_timing.pts - last_observation_pts_;
    if (age > max_horizon_ns_) {
        pending_stats_.timed_out_boxes += display_entries_.size();
        states_.clear();
        display_entries_.clear();
        return;
    }
    const double dt_seconds = static_cast<double>(age) / GST_SECOND;
    std::vector<int> invalid_track_ids;
    output.boxes.reserve(display_entries_.size());
    for (const auto &entry : display_entries_) {
        if (entry.untracked) {
            auto box = *entry.untracked;
            const auto normalized = display_box(box, measurement(box.detection));
            if (normalized.box)
                output.boxes.push_back(*normalized.box);
            if (normalized.clipped)
                ++pending_stats_.clipped_boxes;
            if (normalized.invalid)
                ++pending_stats_.invalid_boxes;
            continue;
        }

        const auto state = states_.find(entry.track_id);
        if (state == states_.end())
            continue;
        SsvOverlayBox box;
        box.detection = state->second.detection;
        box.track_id = entry.track_id;
        box.track_state = state->second.track_state;
        box.occluded = state->second.occluded;

        std::array<double, 4> predicted{};
        if (motion_prediction_ && age > 0 &&
            state->second.observations > 1) {
            for (std::size_t axis = 0; axis < state->second.axes.size(); ++axis) {
                auto copy = state->second.axes[axis];
                predict_axis(copy, dt_seconds);
                predicted[axis] = copy.position;
            }
            box.predicted = true;
        } else {
            predicted = measurement(state->second.detection);
        }

        const auto normalized = display_box(box, predicted);
        if (normalized.box) {
            output.boxes.push_back(*normalized.box);
            if (normalized.clipped)
                ++pending_stats_.clipped_boxes;
        } else {
            if (normalized.invalid) {
                ++pending_stats_.invalid_boxes;
                invalid_track_ids.push_back(entry.track_id);
            }
        }
    }
    for (const int track_id : invalid_track_ids)
        states_.erase(track_id);
}

OverlayMotionPredictorStats OverlayMotionPredictor::take_stats()
{
    const auto stats = pending_stats_;
    pending_stats_ = {};
    return stats;
}

void OverlayMotionPredictor::reset()
{
    source_id_.clear();
    generation_ = 0;
    observation_timing_ = {};
    last_observation_pts_ = GST_CLOCK_TIME_NONE;
    states_.clear();
    display_entries_.clear();
    pending_stats_ = {};
}
