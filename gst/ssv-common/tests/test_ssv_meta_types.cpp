#include "ssv_meta.hpp"

#include <cassert>
#include <concepts>
#include <cstdio>
#include <type_traits>

namespace {

template <typename T>
concept HasTrackId = requires(T value) { value.track_id; };

template <typename T>
concept HasTiming = requires(T value) { value.timing; };

static_assert(!HasTrackId<SsvDetection>);
static_assert(!HasTiming<SsvDetection>);
static_assert(HasTrackId<SsvTrackedObject>);
static_assert(std::is_same_v<decltype(SsvFrameTiming::generation), std::uint64_t>);

SsvDetection make_detection(float x1 = 0.1F, float x2 = 0.3F)
{
    SsvDetection detection{};
    std::snprintf(detection.class_name, sizeof(detection.class_name), "person");
    detection.confidence = 0.9F;
    detection.x1 = x1;
    detection.y1 = 0.2F;
    detection.x2 = x2;
    detection.y2 = 0.4F;
    detection.class_id = 0;
    return detection;
}

void test_frame_timing_defaults()
{
    SsvFrameTiming timing;
    assert(timing.pts == GST_CLOCK_TIME_NONE);
    assert(timing.duration == GST_CLOCK_TIME_NONE);
    assert(timing.generation == 0);
}

void test_meta_types_compose_identity_timing_and_tracking_truth()
{
    SsvDetectionFrame detections;
    detections.frame_id = 0;
    detections.source_id = "camera-01";
    detections.timing = {5 * GST_SECOND, GST_SECOND / 5, 7};
    detections.detections.push_back(make_detection());

    SsvTrackedObject object;
    object.detection = detections.detections.front();
    object.track_id = 42;
    object.track_state = SSV_TRACK_MATCHED;
    object.occluded = true;

    SsvTrackedFrame tracked;
    tracked.frame_id = detections.frame_id;
    tracked.source_id = detections.source_id;
    tracked.timing = detections.timing;
    tracked.objects.push_back(object);
    assert(tracked.frame_id == 0);
    assert(tracked.source_id == "camera-01");
    assert(tracked.timing.pts == 5 * GST_SECOND);
    assert(tracked.timing.duration == GST_SECOND / 5);
    assert(tracked.timing.generation == 7);
    assert(tracked.objects.size() == 1);
    assert(tracked.objects.front().track_id == 42);
    assert(tracked.objects.front().track_state == SSV_TRACK_MATCHED);
    assert(tracked.objects.front().occluded);
}

} // namespace

int main()
{
    test_frame_timing_defaults();
    test_meta_types_compose_identity_timing_and_tracking_truth();
    return 0;
}
