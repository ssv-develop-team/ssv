#include "overlay_presentation.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

OverlayPresentationModel::OverlayPresentationModel(
    std::shared_ptr<SsvSourceMeta> meta,
    bool motion_prediction,
    std::uint32_t max_horizon_ms)
    : meta_(std::move(meta))
    , predictor_(motion_prediction, max_horizon_ms)
{
    if (!meta_)
        throw std::invalid_argument("source meta must not be null");
}

SsvOverlayFrame OverlayPresentationModel::present(
    const SsvFrameTiming &display_timing)
{
    SsvOverlayFrame output;
    present_into(display_timing, output);
    return output;
}

void OverlayPresentationModel::present_into(
    const SsvFrameTiming &display_timing,
    SsvOverlayFrame &output)
{
    ++stats_.display_frames;
    const auto source_id = meta_->source_id();
    output.source_id = source_id;
    output.observation_timing = {};
    output.display_timing = display_timing;
    output.boxes.clear();
    if (display_timing.pts == GST_CLOCK_TIME_NONE) {
        ++stats_.no_pts_frames;
        return;
    }
    if (display_generation_ != display_timing.generation) {
        predictor_.reset();
        last_observed_.reset();
        display_generation_ = display_timing.generation;
    }

    auto snapshot = meta_->latest_tracked_at_or_before(display_timing.pts);
    if (!snapshot) {
        ++stats_.history_misses;
        return;
    }
    if (snapshot->source_id != source_id
        || snapshot->timing.generation != display_timing.generation
        || snapshot->timing.pts == GST_CLOCK_TIME_NONE
        || snapshot->timing.pts > display_timing.pts) {
        ++stats_.future_matches;
        return;
    }
    ++stats_.history_hits;

    if (!last_observed_ || last_observed_.get() != snapshot.get()) {
        if (predictor_.observe(snapshot))
            ++stats_.observation_snapshots;
        last_observed_ = snapshot;
    }
    if (meta_->generation() != display_timing.generation)
        return;

    predictor_.predict_into(source_id, display_timing, output);
    const auto predictor_stats = predictor_.take_stats();
    stats_.timed_out_boxes += predictor_stats.timed_out_boxes;
    stats_.clipped_boxes += predictor_stats.clipped_boxes;
    stats_.invalid_boxes += predictor_stats.invalid_boxes;
    for (const auto &box : output.boxes) {
        if (box.predicted)
            ++stats_.predicted_boxes;
        else
            ++stats_.observed_boxes;
    }
    if (!output.boxes.empty()) {
        const auto age = display_timing.pts - snapshot->timing.pts;
        stats_.prediction_age_total_ns += age;
        stats_.max_prediction_age_ns = std::max(
            stats_.max_prediction_age_ns, age);
    }
    stats_.max_predictor_states = std::max(
        stats_.max_predictor_states, predictor_.state_count());
}

void OverlayPresentationModel::reset()
{
    predictor_.reset();
    last_observed_.reset();
    display_generation_ = 0;
}
