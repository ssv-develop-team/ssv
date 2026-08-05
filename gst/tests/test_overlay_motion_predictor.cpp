#include "overlay_motion_predictor.hpp"
#include "ssv_meta.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace {

SsvTrackedObject make_object(
    int track_id,
    float center_x,
    float center_y = 0.5F,
    float width = 0.2F,
    float height = 0.2F,
    int class_id = 0,
    SsvTrackState state = SSV_TRACK_MATCHED)
{
    SsvTrackedObject object;
    std::snprintf(
        object.detection.class_name,
        sizeof(object.detection.class_name),
        class_id == 0 ? "person" : "helmet");
    object.detection.confidence = 0.9F;
    object.detection.x1 = center_x - width / 2.0F;
    object.detection.y1 = center_y - height / 2.0F;
    object.detection.x2 = center_x + width / 2.0F;
    object.detection.y2 = center_y + height / 2.0F;
    object.detection.class_id = class_id;
    object.track_id = track_id;
    object.track_state = state;
    return object;
}

std::shared_ptr<const SsvTrackedFrame> make_frame(
    GstClockTime pts,
    std::vector<SsvTrackedObject> objects,
    std::uint64_t generation = 1,
    const char *source_id = "camera-01")
{
    SsvTrackedFrame frame;
    frame.source_id = source_id;
    frame.timing = {pts, GST_SECOND / 5, generation};
    frame.objects = std::move(objects);
    return std::make_shared<const SsvTrackedFrame>(std::move(frame));
}

float center_x(const SsvOverlayBox &box)
{
    return (box.detection.x1 + box.detection.x2) / 2.0F;
}

float intersection_over_union(const SsvOverlayBox &box, float truth_center_x)
{
    const float truth_x1 = truth_center_x - 0.1F;
    const float truth_x2 = truth_center_x + 0.1F;
    const float overlap = std::max(
        0.0F,
        std::min(box.detection.x2, truth_x2) -
            std::max(box.detection.x1, truth_x1));
    const float box_width = box.detection.x2 - box.detection.x1;
    return overlap * 0.2F / (box_width * 0.2F + 0.04F - overlap * 0.2F);
}

void test_first_observation_and_repeated_prediction_do_not_mutate_state()
{
    OverlayMotionPredictor predictor(true, 300);
    assert(predictor.observe(make_frame(0, {make_object(7, 0.2F)})));
    auto first = predictor.predict(
        "camera-01", {100 * GST_MSECOND, GST_SECOND / 30, 1});
    auto repeated = predictor.predict(
        "camera-01", {100 * GST_MSECOND, GST_SECOND / 30, 1});
    assert(first.boxes.size() == 1);
    assert(std::fabs(center_x(first.boxes.front()) - 0.2F) < 0.0001F);
    assert(!first.boxes.front().predicted);
    assert(center_x(first.boxes.front()) == center_x(repeated.boxes.front()));
    assert(predictor.state_count() == 1);

    assert(predictor.observe(
        make_frame(200 * GST_MSECOND, {make_object(7, 0.24F)})));
    auto predicted = predictor.predict(
        "camera-01", {300 * GST_MSECOND, GST_SECOND / 30, 1});
    auto predicted_again = predictor.predict(
        "camera-01", {300 * GST_MSECOND, GST_SECOND / 30, 1});
    assert(predicted.boxes.size() == 1);
    assert(center_x(predicted.boxes.front()) > 0.255F);
    assert(center_x(predicted.boxes.front()) ==
           center_x(predicted_again.boxes.front()));
}

void test_horizon_boundary_and_disabled_hold_last()
{
    OverlayMotionPredictor predictor(true, 300);
    predictor.observe(make_frame(GST_SECOND, {make_object(1, 0.3F)}));
    assert(predictor.predict(
               "camera-01", {GST_SECOND + 300 * GST_MSECOND, 0, 1})
               .boxes.size() == 1);
    assert(predictor.predict(
               "camera-01", {GST_SECOND + 301 * GST_MSECOND, 0, 1})
               .boxes.empty());
    assert(predictor.state_count() == 0);
    const auto timed_out_stats = predictor.take_stats();
    assert(timed_out_stats.timed_out_boxes == 1);
    assert(timed_out_stats.clipped_boxes == 0);
    assert(timed_out_stats.invalid_boxes == 0);

    OverlayMotionPredictor hold_last(false, 50);
    hold_last.observe(make_frame(0, {make_object(2, 0.2F)}));
    hold_last.observe(make_frame(20 * GST_MSECOND, {make_object(2, 0.4F)}));
    auto held = hold_last.predict(
        "camera-01", {50 * GST_MSECOND, 0, 1});
    assert(held.boxes.size() == 1);
    assert(std::fabs(center_x(held.boxes.front()) - 0.4F) < 0.0001F);
    assert(!held.boxes.front().predicted);
    assert(hold_last.predict(
               "camera-01", {71 * GST_MSECOND, 0, 1})
               .boxes.empty());
}

void test_large_observation_gap_reinitializes_state()
{
    OverlayMotionPredictor predictor(true, 300);
    assert(predictor.observe(make_frame(0, {make_object(3, 0.2F)})));
    assert(predictor.observe(
        make_frame(100 * GST_MSECOND, {make_object(3, 0.3F)})));

    assert(predictor.observe(
        make_frame(401 * GST_MSECOND, {make_object(3, 0.5F)})));
    const auto after_gap = predictor.predict(
        "camera-01", {501 * GST_MSECOND, 0, 1});

    assert(after_gap.boxes.size() == 1);
    assert(std::fabs(center_x(after_gap.boxes.front()) - 0.5F) < 0.001F);
    assert(!after_gap.boxes.front().predicted);
    assert(predictor.state_count() == 1);
    assert(predictor.take_stats().timed_out_boxes == 1);
}

void test_predict_into_reuses_output_capacity()
{
    OverlayMotionPredictor predictor(true, 300);
    assert(predictor.observe(make_frame(0, {make_object(6, 0.4F)})));

    SsvOverlayFrame output;
    predictor.predict_into(
        "camera-01", {50 * GST_MSECOND, 0, 1}, output);
    assert(output.boxes.size() == 1);
    const auto capacity = output.boxes.capacity();

    predictor.predict_into(
        "camera-01", {100 * GST_MSECOND, 0, 1}, output);
    assert(output.boxes.size() == 1);
    assert(output.boxes.capacity() == capacity);
}

void test_complete_snapshots_clean_missing_invalid_and_terminal_tracks()
{
    OverlayMotionPredictor predictor(true, 300);
    predictor.observe(make_frame(
        0, {make_object(1, 0.2F), make_object(2, 0.4F)}));
    assert(predictor.state_count() == 2);

    predictor.observe(make_frame(
        100 * GST_MSECOND, {make_object(2, 0.42F)}));
    assert(predictor.state_count() == 1);

    auto terminal = make_object(2, 0.42F);
    terminal.track_state = SSV_TRACK_LOST;
    predictor.observe(make_frame(200 * GST_MSECOND, {terminal}));
    assert(predictor.state_count() == 0);

    auto no_id = make_object(-1, 0.6F);
    predictor.observe(make_frame(300 * GST_MSECOND, {no_id}));
    assert(predictor.state_count() == 0);
    assert(predictor.predict(
               "camera-01", {350 * GST_MSECOND, 0, 1})
               .boxes.size() == 1);

    predictor.observe(make_frame(400 * GST_MSECOND, {}));
    assert(predictor.state_count() == 0);
    assert(predictor.predict(
               "camera-01", {400 * GST_MSECOND, 0, 1})
               .boxes.empty());
}

void test_generation_order_class_change_and_invalid_bbox_reset_state()
{
    OverlayMotionPredictor predictor(true, 300);
    assert(predictor.observe(make_frame(100 * GST_MSECOND, {make_object(4, 0.2F)})));
    assert(!predictor.observe(make_frame(100 * GST_MSECOND, {make_object(4, 0.8F)})));
    assert(!predictor.observe(make_frame(50 * GST_MSECOND, {make_object(4, 0.8F)})));

    predictor.observe(make_frame(200 * GST_MSECOND, {make_object(4, 0.3F)}));
    predictor.observe(make_frame(
        300 * GST_MSECOND, {make_object(4, 0.7F, 0.5F, 0.2F, 0.2F, 1)}));
    auto class_reset = predictor.predict(
        "camera-01", {350 * GST_MSECOND, 0, 1});
    assert(class_reset.boxes.size() == 1);
    assert(std::fabs(center_x(class_reset.boxes.front()) - 0.7F) < 0.001F);

    auto invalid = make_object(4, 0.7F);
    invalid.detection.x1 = std::numeric_limits<float>::quiet_NaN();
    predictor.observe(make_frame(400 * GST_MSECOND, {invalid}));
    assert(predictor.state_count() == 0);
    const auto invalid_stats = predictor.take_stats();
    assert(invalid_stats.timed_out_boxes == 0);
    assert(invalid_stats.clipped_boxes == 0);
    assert(invalid_stats.invalid_boxes == 1);

    predictor.observe(make_frame(
        10 * GST_MSECOND, {make_object(5, 0.4F)}, 2));
    assert(predictor.predict(
               "camera-01", {20 * GST_MSECOND, 0, 1})
               .boxes.empty());
    assert(predictor.predict(
               "camera-02", {20 * GST_MSECOND, 0, 2})
               .boxes.empty());
    assert(predictor.predict(
               "camera-01", {20 * GST_MSECOND, 0, 2})
               .boxes.size() == 1);
}

void test_prediction_clamps_only_display_output_and_hides_fully_outside()
{
    OverlayMotionPredictor predictor(true, 300);
    predictor.observe(make_frame(0, {make_object(9, 0.2F)}));
    predictor.observe(make_frame(200 * GST_MSECOND, {make_object(9, 0.8F)}));

    auto partial = predictor.predict(
        "camera-01", {250 * GST_MSECOND, 0, 1});
    assert(partial.boxes.size() == 1);
    assert(partial.boxes.front().detection.x2 == 1.0F);
    const auto clipped_stats = predictor.take_stats();
    assert(clipped_stats.timed_out_boxes == 0);
    assert(clipped_stats.clipped_boxes == 1);
    assert(clipped_stats.invalid_boxes == 0);

    auto outside = predictor.predict(
        "camera-01", {350 * GST_MSECOND, 0, 1});
    assert(outside.boxes.empty());
    assert(predictor.state_count() == 1);
    const auto outside_stats = predictor.take_stats();
    assert(outside_stats.timed_out_boxes == 0);
    assert(outside_stats.clipped_boxes == 0);
    assert(outside_stats.invalid_boxes == 0);

    auto observation = predictor.predict(
        "camera-01", {200 * GST_MSECOND, 0, 1});
    assert(observation.boxes.size() == 1);
    assert(std::fabs(center_x(observation.boxes.front()) - 0.8F) < 0.001F);
}

void test_synthetic_motion_beats_hold_last_and_meets_iou_gate()
{
    OverlayMotionPredictor predictor(true, 300);
    double predicted_error = 0.0;
    double hold_error = 0.0;
    double iou_sum = 0.0;
    std::size_t samples = 0;
    float latest_observation = 0.2F;

    for (int display_index = 0; display_index <= 60; ++display_index) {
        const auto display_pts =
            static_cast<GstClockTime>(display_index) * GST_SECOND / 30;
        const float seconds = static_cast<float>(display_pts) / GST_SECOND;
        const float truth_center = 0.2F + 0.2F * seconds;
        if (display_index % 6 == 0) {
            latest_observation = truth_center;
            predictor.observe(make_frame(
                display_pts, {make_object(11, truth_center)}));
        }
        if (display_index < 6)
            continue;

        auto output = predictor.predict(
            "camera-01", {display_pts, GST_SECOND / 30, 1});
        assert(output.boxes.size() == 1);
        predicted_error += std::fabs(center_x(output.boxes.front()) - truth_center);
        hold_error += std::fabs(latest_observation - truth_center);
        iou_sum += intersection_over_union(output.boxes.front(), truth_center);
        ++samples;
    }

    assert(predicted_error <= hold_error * 0.5);
    assert(iou_sum / static_cast<double>(samples) >= 0.90);
}

void test_one_hundred_target_history_and_prediction_p99_under_one_ms()
{
    auto source = std::make_shared<SsvSourceMeta>("camera-01");
    SsvTimelineCursor timeline(source);
    const auto segment = timeline.on_segment({0, 0, 0, 1.0});
    std::vector<SsvTrackedObject> objects;
    objects.reserve(100);
    for (int id = 0; id < 100; ++id) {
        const float x = 0.05F + static_cast<float>(id % 10) * 0.09F;
        const float y = 0.05F + static_cast<float>(id / 10) * 0.09F;
        objects.push_back(make_object(id, x, y, 0.04F, 0.04F));
    }
    SsvDetectionFrame observation;
    observation.source_id = "camera-01";
    observation.timing = {
        GST_SECOND, GST_SECOND / 5, segment.generation};
    assert(source->publish_tracked(std::move(observation), std::move(objects)) ==
           SsvMetaResult::Published);

    OverlayMotionPredictor predictor(true, 300);
    std::vector<double> timings_us;
    timings_us.reserve(500);
    for (int iteration = 0; iteration < 500; ++iteration) {
        const auto started = std::chrono::steady_clock::now();
        auto snapshot = source->latest_tracked_at_or_before(GST_SECOND);
        predictor.observe(snapshot);
        auto output = predictor.predict(
            "camera-01",
            {GST_SECOND + 100 * GST_MSECOND, 0, segment.generation});
        assert(output.boxes.size() == 100);
        const auto elapsed = std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - started).count();
        timings_us.push_back(elapsed);
    }
    std::sort(timings_us.begin(), timings_us.end());
    const double p99 = timings_us[494];
    std::printf("100-target history+predictor p99: %.2f us\n", p99);
    assert(p99 < 1000.0);
}

} // namespace

int main()
{
    test_first_observation_and_repeated_prediction_do_not_mutate_state();
    test_horizon_boundary_and_disabled_hold_last();
    test_large_observation_gap_reinitializes_state();
    test_predict_into_reuses_output_capacity();
    test_complete_snapshots_clean_missing_invalid_and_terminal_tracks();
    test_generation_order_class_change_and_invalid_bbox_reset_state();
    test_prediction_clamps_only_display_output_and_hides_fully_outside();
    test_synthetic_motion_beats_hold_last_and_meets_iou_gate();
    test_one_hundred_target_history_and_prediction_p99_under_one_ms();
    return 0;
}
