#include "botsort_tracker.hpp"

#include <cassert>
#include <vector>

namespace {

botsort::Detection make_detection(
    float x1 = 0.1F,
    float y1 = 0.2F,
    float x2 = 0.3F,
    float y2 = 0.5F,
    float score = 0.9F)
{
    botsort::Detection detection;
    detection.x1 = x1;
    detection.y1 = y1;
    detection.x2 = x2;
    detection.y2 = y2;
    detection.score = score;
    detection.class_id = 0;
    detection.class_name = "person";
    return detection;
}

void test_new_track_matches_and_recovers_after_one_missed_frame()
{
    botsort::TrackerConfig config;
    config.gmc_method = botsort::GmcMethod::kNone;
    botsort::BoTSortTracker tracker(config);

    const auto first = tracker.update({make_detection()});
    assert(first.detections.size() == 1);
    assert(first.detections.front().track_id == 1);
    assert(first.detections.front().track_state == botsort::TrackState::kNew);
    assert(first.stats.new_tracks == 1);

    const auto matched = tracker.update({make_detection(0.11F, 0.2F, 0.31F, 0.5F)});
    assert(matched.detections.size() == 1);
    assert(matched.detections.front().track_id == 1);
    assert(matched.detections.front().track_state == botsort::TrackState::kMatched);
    assert(matched.stats.matched_first_stage == 1);

    const auto missed = tracker.update({});
    assert(missed.detections.empty());
    assert(missed.stats.lost_tracks == 1);

    const auto recovered = tracker.update({make_detection(0.12F, 0.2F, 0.32F, 0.5F)});
    assert(recovered.detections.size() == 1);
    assert(recovered.detections.front().track_id == 1);
    assert(recovered.detections.front().track_state == botsort::TrackState::kMatched);
    assert(recovered.stats.matched_first_stage == 1);
}

void test_late_track_requires_confirmation()
{
    botsort::TrackerConfig config;
    config.gmc_method = botsort::GmcMethod::kNone;

    botsort::BoTSortTracker confirming_tracker(config);
    static_cast<void>(confirming_tracker.update({make_detection()}));
    const auto pending = confirming_tracker.update({
        make_detection(),
        make_detection(0.6F, 0.1F, 0.8F, 0.4F),
    });
    assert(pending.detections.size() == 2);
    assert(pending.detections[0].track_id == 1);
    assert(pending.detections[1].track_id == 2);
    assert(pending.detections[1].track_state == botsort::TrackState::kNew);
    assert(pending.stats.new_tracks == 1);

    const auto confirmed = confirming_tracker.update({
        make_detection(),
        make_detection(0.6F, 0.1F, 0.8F, 0.4F),
    });
    assert(confirmed.detections.size() == 2);
    assert(confirmed.detections[0].track_id == 1);
    assert(confirmed.detections[1].track_id == 2);
    assert(confirmed.detections[1].track_state == botsort::TrackState::kMatched);

    botsort::BoTSortTracker removing_tracker(config);
    static_cast<void>(removing_tracker.update({make_detection()}));
    static_cast<void>(removing_tracker.update({
        make_detection(),
        make_detection(0.6F, 0.1F, 0.8F, 0.4F),
    }));
    const auto removed = removing_tracker.update({make_detection()});
    assert(removed.detections.size() == 1);
    assert(removed.detections.front().track_id == 1);
    assert(removed.stats.removed_tracks == 1);
}

void test_lost_track_is_removed_after_buffer_expires()
{
    botsort::TrackerConfig config;
    config.gmc_method = botsort::GmcMethod::kNone;
    config.track_buffer = 1;
    botsort::BoTSortTracker tracker(config);

    static_cast<void>(tracker.update({make_detection()}));
    const auto lost = tracker.update({});
    assert(lost.stats.lost_tracks == 1);

    const auto removed = tracker.update({});
    assert(removed.detections.empty());
    assert(removed.stats.removed_tracks == 1);
}

void test_invalid_detections_are_filtered_and_input_order_is_restored()
{
    botsort::TrackerConfig config;
    config.gmc_method = botsort::GmcMethod::kNone;
    botsort::BoTSortTracker tracker(config);

    auto invalid = make_detection();
    invalid.x2 = invalid.x1;
    const auto result = tracker.update({
        make_detection(0.6F, 0.1F, 0.8F, 0.4F),
        invalid,
        make_detection(),
    });

    assert(result.stats.input_count == 3);
    assert(result.stats.filtered_count == 2);
    assert(result.detections.size() == 2);
    assert(result.detections[0].input_index == 0);
    assert(result.detections[1].input_index == 2);
    assert(result.detections[0].track_id == 1);
    assert(result.detections[1].track_id == 2);
}

void test_optional_gmc_input_contract()
{
    botsort::TrackerConfig config;
#if SSV_HAS_OPENCV
    config.gmc_method = botsort::GmcMethod::kSparseOptFlow;
#else
    config.gmc_method = botsort::GmcMethod::kNone;
#endif
    botsort::BoTSortTracker tracker(config);

    const auto result = tracker.update(
        {make_detection()}, std::nullopt);
    assert(result.detections.size() == 1);
    assert(!result.stats.gmc_applied);
#if SSV_HAS_OPENCV
    assert(result.stats.gmc_fallback_identity);
#else
    assert(!result.stats.gmc_fallback_identity);
#endif
}

} // namespace

int main()
{
    test_new_track_matches_and_recovers_after_one_missed_frame();
    test_late_track_requires_confirmation();
    test_lost_track_is_removed_after_buffer_expires();
    test_invalid_detections_are_filtered_and_input_order_is_restored();
    test_optional_gmc_input_contract();
    return 0;
}
