#include "overlay_presentation.hpp"

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
    object.track_id = 7;
    object.track_state = SSV_TRACK_MATCHED;
    assert(meta->publish_tracked(
               std::move(observation), {std::move(object)}) ==
           SsvMetaResult::Published);
}

void test_presentation_keeps_causal_fresh_generation_scoped_snapshots()
{
    const std::string source_id = "presentation-causal-test";
    auto meta = std::make_shared<SsvSourceMeta>(source_id);
    assert(meta->source_id() == source_id);
    SsvTimelineCursor timeline(meta);
    const auto segment = timeline.on_segment({0, 0, 0, 1.0});
    OverlayPresentationModel presentation(meta, true, 300);
    publish_tracked_frame(
        meta, source_id, GST_SECOND, segment.generation);

    assert(presentation.present(
        {500 * GST_MSECOND, GST_SECOND / 30, segment.generation})
               .boxes.empty());
    const auto matched = presentation.present(
        {GST_SECOND + 100 * GST_MSECOND,
         GST_SECOND / 30,
         segment.generation});
    assert(matched.boxes.size() == 1);
    assert(matched.source_id == source_id);
    assert(matched.observation_timing.pts == GST_SECOND);
    assert(presentation.present(
        {GST_SECOND + 150 * GST_MSECOND,
         GST_SECOND / 30,
         segment.generation})
               .boxes.size() == 1);
    assert(presentation.stats().observation_snapshots == 1);

    assert(presentation.present(
        {GST_SECOND + 301 * GST_MSECOND,
         GST_SECOND / 30,
         segment.generation})
               .boxes.empty());
    const auto reset = timeline.on_lifecycle_reset();
    assert(reset.generation == segment.generation + 1);
    assert(presentation.present(
        {2 * GST_SECOND, GST_SECOND / 30, reset.generation})
               .boxes.empty());
}

} // namespace

int main()
{
    test_presentation_keeps_causal_fresh_generation_scoped_snapshots();
    return 0;
}
