#include "ssv_meta.hpp"

#include <cassert>
#include <memory>

namespace {

void test_meta_timeline_segment_and_concurrent_reset_are_idempotent()
{
    auto source = std::make_shared<SsvSourceMeta>("camera-01");
    SsvTimelineCursor infer(source);
    SsvTimelineCursor overlay(source);
    const SsvTimelineSegment first{0, 0, 0, 1.0};

    const auto infer_segment = infer.on_segment(first);
    const auto overlay_segment = overlay.on_segment(first);

    assert(infer_segment.reset);
    assert(overlay_segment.reset);
    assert(infer_segment.generation == 1);
    assert(overlay_segment.generation == 1);
    assert(source->generation() == 1);
    assert(source->stats().generation_resets == 1);

    infer.on_buffer(GST_SECOND, false);
    overlay.on_buffer(GST_SECOND, false);
    const SsvTimelineSegment second{5 * GST_SECOND, 0, 0, 1.0};
    const auto infer_second = infer.on_segment(second);
    const auto overlay_second = overlay.on_segment(second);
    assert(infer_second.generation == 2);
    assert(overlay_second.generation == 2);
    assert(source->generation() == 2);
    assert(source->stats().generation_resets == 2);
}

void test_flush_discontinuity_and_pts_rollback_reset_once()
{
    auto source = std::make_shared<SsvSourceMeta>("camera-01");
    SsvTimelineCursor cursor(source);
    cursor.on_segment({0, 0, 0, 1.0});

    const auto first = cursor.on_buffer(2 * GST_SECOND, false);
    const auto duplicate = cursor.on_buffer(2 * GST_SECOND, false);
    const auto gap = cursor.on_buffer(20 * GST_SECOND, false);
    assert(first.generation == 1 && !first.reset);
    assert(duplicate.generation == 1 && !duplicate.reset);
    assert(gap.generation == 1 && !gap.reset);

    const auto rollback = cursor.on_buffer(GST_SECOND, false);
    assert(rollback.generation == 2 && rollback.reset);

    const auto discont = cursor.on_buffer(2 * GST_SECOND, true);
    assert(discont.generation == 3 && discont.reset);

    const auto flush = cursor.on_flush_stop(true);
    const auto repeated_flush = cursor.on_flush_stop(true);
    assert(flush.generation == 4 && flush.reset);
    assert(repeated_flush.generation == 4 && repeated_flush.reset);
    assert(cursor.on_flush_stop(false).generation == 4);
    assert(source->stats().generation_resets == 4);
}

void test_segment_after_flush_is_coalesced_into_the_same_generation()
{
    auto source = std::make_shared<SsvSourceMeta>("camera-01");
    SsvTimelineCursor cursor(source);
    cursor.on_segment({0, 0, 0, 1.0});
    cursor.on_buffer(GST_SECOND, false);

    const auto flush = cursor.on_flush_stop(true);
    const auto segment = cursor.on_segment({5 * GST_SECOND, 0, 0, 1.0});

    assert(flush.generation == 2 && flush.reset);
    assert(segment.generation == 2 && !segment.reset);
    assert(source->generation() == 2);
    assert(source->stats().generation_resets == 2);
}

void test_cursor_synchronizes_to_reset_from_other_branch()
{
    auto source = std::make_shared<SsvSourceMeta>("camera-01");
    SsvTimelineCursor infer(source);
    SsvTimelineCursor overlay(source);
    infer.on_segment({0, 0, 0, 1.0});
    overlay.on_segment({0, 0, 0, 1.0});
    infer.on_buffer(GST_SECOND, false);
    overlay.on_buffer(GST_SECOND, false);

    const auto reset = infer.on_buffer(2 * GST_SECOND, true);
    const auto synchronized = overlay.on_buffer(2 * GST_SECOND, true);

    assert(reset.generation == 2 && reset.reset);
    assert(synchronized.generation == 2 && synchronized.reset);
    assert(source->generation() == 2);
    assert(source->stats().generation_resets == 2);
}

void test_plugin_lifecycle_reset_is_shared_by_both_branches()
{
    auto source = std::make_shared<SsvSourceMeta>("camera-01");
    SsvTimelineCursor infer(source);
    SsvTimelineCursor overlay(source);
    const SsvTimelineSegment segment{0, 0, 0, 1.0};
    infer.on_segment(segment);
    overlay.on_segment(segment);

    const auto infer_stop = infer.on_lifecycle_reset();
    const auto overlay_stop = overlay.on_lifecycle_reset();
    assert(infer_stop.reset && infer_stop.generation == 2);
    assert(overlay_stop.reset && overlay_stop.generation == 2);
    assert(source->stats().generation_resets == 2);

    SsvTimelineCursor restarted_infer(source);
    SsvTimelineCursor restarted_overlay(source);
    const auto infer_segment = restarted_infer.on_segment(segment);
    const auto overlay_segment = restarted_overlay.on_segment(segment);
    assert(infer_segment.reset && infer_segment.generation == 2);
    assert(overlay_segment.reset && overlay_segment.generation == 2);
    assert(source->stats().generation_resets == 2);
}

} // namespace

int main()
{
    test_meta_timeline_segment_and_concurrent_reset_are_idempotent();
    test_flush_discontinuity_and_pts_rollback_reset_once();
    test_segment_after_flush_is_coalesced_into_the_same_generation();
    test_cursor_synchronizes_to_reset_from_other_branch();
    test_plugin_lifecycle_reset_is_shared_by_both_branches();
    return 0;
}
