#include "overlay_runtime.hpp"

#include <cassert>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>

namespace {

void publish_tracked_frame(
    const std::shared_ptr<SsvSourceMeta> &meta,
    const std::string &source_id,
    GstClockTime pts,
    std::uint64_t generation)
{
    SsvDetectionFrame observation;
    observation.source_id = source_id;
    observation.timing = {pts, GST_SECOND / 5, generation};
    SsvTrackedObject object;
    std::snprintf(
        object.detection.class_name,
        sizeof(object.detection.class_name),
        "person");
    object.detection.confidence = 0.9F;
    object.detection.x1 = 0.1F;
    object.detection.y1 = 0.2F;
    object.detection.x2 = 0.3F;
    object.detection.y2 = 0.4F;
    object.detection.class_id = 0;
    object.track_id = 7;
    object.track_state = SSV_TRACK_MATCHED;
    assert(meta->publish_tracked(
               std::move(observation), {std::move(object)}) ==
           SsvMetaResult::Published);
}

void test_runtime_uses_causal_history_and_observes_snapshot_once()
{
    const std::string source_id = "runtime-causal-test";
    auto source_context = std::make_shared<SsvSourceContext>(source_id);
    OverlayRuntime runtime(source_context->meta(), true, 300);
    const auto segment = runtime.on_segment({0, 0, 0, 1.0});
    assert(segment.generation > 0);
    auto meta = source_context->meta();
    publish_tracked_frame(
        meta, source_id, GST_SECOND, segment.generation);

    auto before_timing = runtime.on_buffer(
        500 * GST_MSECOND, GST_SECOND / 30, false);
    assert(runtime.frame_for_display(before_timing).boxes.empty());

    auto matched_timing = runtime.on_buffer(
        GST_SECOND + 100 * GST_MSECOND, GST_SECOND / 30, false);
    auto matched = runtime.frame_for_display(matched_timing);
    assert(matched.boxes.size() == 1);
    assert(matched.observation_timing.pts == GST_SECOND);
    assert(matched.display_timing.pts == GST_SECOND + 100 * GST_MSECOND);

    auto repeated_timing = runtime.on_buffer(
        GST_SECOND + 150 * GST_MSECOND, GST_SECOND / 30, false);
    assert(runtime.frame_for_display(repeated_timing).boxes.size() == 1);
    const auto stats = runtime.stats();
    assert(stats.history_hits == 2);
    assert(stats.history_misses == 1);
    assert(stats.observation_snapshots == 1);
    assert(stats.future_matches == 0);
}

void test_runtime_hides_no_pts_and_resets_on_discontinuity()
{
    const std::string source_id = "runtime-reset-test";
    auto source_context = std::make_shared<SsvSourceContext>(source_id);
    OverlayRuntime runtime(source_context->meta(), false, 100);
    const auto segment = runtime.on_segment({0, 0, 0, 1.0});
    auto meta = source_context->meta();
    publish_tracked_frame(
        meta, source_id, 100 * GST_MSECOND, segment.generation);

    const auto no_pts = runtime.on_buffer(
        GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE, false);
    assert(runtime.frame_for_display(no_pts).boxes.empty());
    assert(runtime.stats().no_pts_frames == 1);

    const auto timed_out = runtime.on_buffer(
        250 * GST_MSECOND, GST_SECOND / 30, false);
    assert(runtime.frame_for_display(timed_out).boxes.empty());
    assert(runtime.stats().timed_out_boxes == 1);
    assert(runtime.stats().prediction_age_total_ns == 0);
    assert(runtime.stats().max_prediction_age_ns == 0);

    const auto reset_timing = runtime.on_buffer(
        50 * GST_MSECOND, GST_SECOND / 30, true);
    assert(reset_timing.generation == segment.generation + 1);
    assert(runtime.frame_for_display(reset_timing).boxes.empty());
    assert(meta->latest_tracked_at_or_before(GST_SECOND) == nullptr);
}

} // namespace

int main()
{
    test_runtime_uses_causal_history_and_observes_snapshot_once();
    test_runtime_hides_no_pts_and_resets_on_discontinuity();
    return 0;
}
