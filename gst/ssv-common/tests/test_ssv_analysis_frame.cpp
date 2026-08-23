#include "ssv_meta.hpp"

#include <gst/gst.h>
#include <gst/video/video.h>

#include <array>
#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <string>

struct TestFailureMemory {
    GstMemory parent;
};

struct TestFailureAllocator {
    GstAllocator parent;
};

struct TestFailureAllocatorClass {
    GstAllocatorClass parent_class;
};

G_DEFINE_TYPE(
    TestFailureAllocator,
    test_failure_allocator,
    GST_TYPE_ALLOCATOR)

static gpointer test_failure_memory_map(
    GstMemory *,
    gsize,
    GstMapFlags)
{
    return nullptr;
}

static void test_failure_memory_unmap(GstMemory *) {}

static GstMemory *test_failure_allocator_alloc(
    GstAllocator *allocator,
    gsize size,
    GstAllocationParams *)
{
    auto *memory = g_new0(TestFailureMemory, 1);
    gst_memory_init(
        &memory->parent,
        GST_MEMORY_FLAG_NO_SHARE,
        allocator,
        nullptr,
        size,
        0,
        0,
        size);
    return &memory->parent;
}

static void test_failure_allocator_free(GstAllocator *, GstMemory *memory)
{
    g_free(memory);
}

static void test_failure_allocator_class_init(
    TestFailureAllocatorClass *klass)
{
    auto *allocator_class = GST_ALLOCATOR_CLASS(klass);
    allocator_class->alloc = test_failure_allocator_alloc;
    allocator_class->free = test_failure_allocator_free;
}

static void test_failure_allocator_init(TestFailureAllocator *self)
{
    auto *allocator = GST_ALLOCATOR(self);
    allocator->mem_type = "TestFailureMemory";
    allocator->mem_map = test_failure_memory_map;
    allocator->mem_unmap = test_failure_memory_unmap;
    GST_OBJECT_FLAG_SET(allocator, GST_ALLOCATOR_FLAG_CUSTOM_ALLOC);
}

namespace {

std::shared_ptr<const SsvAnalysisFrame> make_direct_frame(
    SsvAnalysisFramePool &pool,
    SsvFrameTiming timing)
{
    GstVideoInfo info;
    gst_video_info_init(&info);
    assert(gst_video_info_set_format(&info, GST_VIDEO_FORMAT_RGBA, 3, 2));
    GstBuffer *buffer = gst_buffer_new_allocate(nullptr, 24, nullptr);
    assert(buffer != nullptr);
    auto frame = pool.create(
        buffer,
        info,
        {3, 2, 3, 2, 1.0F, 1.0F, 0, 0, 0, 0},
        timing);
    gst_buffer_unref(buffer);
    return frame;
}

void test_direct_frame_maps_once_and_releases_after_shared_consumers()
{
    GstVideoInfo info;
    gst_video_info_init(&info);
    assert(gst_video_info_set_format(&info, GST_VIDEO_FORMAT_RGBA, 3, 2));

    std::array<std::uint8_t, 24> pixels {};
    GstBuffer *buffer = gst_buffer_new_wrapped_full(
        GST_MEMORY_FLAG_READONLY,
        pixels.data(),
        pixels.size(),
        0,
        pixels.size(),
        nullptr,
        nullptr);
    assert(buffer != nullptr);

    SsvAnalysisFramePool pool(3, 2, 1);
    auto frame = pool.create(
        buffer,
        info,
        {3, 2, 3, 2, 1.0F, 1.0F, 0, 0, 0, 0},
        {GST_SECOND, GST_SECOND / 15, 4});

    assert(frame->view().bytes.data() == pixels.data());
    assert(frame->view().bytes.size() == pixels.size());
    assert(frame->view().stride == 12);
    assert(frame->transform().source_width == 3);
    assert(frame->timing().generation == 4);
    assert(GST_MINI_OBJECT_REFCOUNT_VALUE(buffer) == 2);

    const auto after_create = pool.stats();
    assert(after_create.map_count == 1);
    assert(after_create.direct_frames == 1);
    assert(after_create.staged_frames == 0);
    assert(after_create.active_maps == 1);
    assert(after_create.outstanding_staging_leases == 0);

    auto inference_consumer = frame;
    auto gmc_consumer = frame;
    static_cast<void>(inference_consumer->view());
    static_cast<void>(gmc_consumer->view());
    assert(pool.stats().map_count == 1);

    frame.reset();
    inference_consumer.reset();
    assert(pool.stats().active_maps == 1);
    gmc_consumer.reset();

    const auto released = pool.stats();
    assert(released.active_maps == 0);
    assert(released.outstanding_staging_leases == 0);
    assert(GST_MINI_OBJECT_REFCOUNT_VALUE(buffer) == 1);
    gst_buffer_unref(buffer);
}

void test_padded_rows_use_and_reuse_the_bounded_staging_pool()
{
    GstVideoInfo info;
    gst_video_info_init(&info);
    assert(gst_video_info_set_format(&info, GST_VIDEO_FORMAT_RGBA, 3, 2));
    info.stride[0] = 16;
    info.size = 32;

    std::array<std::uint8_t, 32> padded {};
    for (std::size_t index = 0; index < 12; ++index) {
        padded[index] = static_cast<std::uint8_t>(index + 1);
        padded[16 + index] = static_cast<std::uint8_t>(index + 21);
    }
    GstBuffer *buffer = gst_buffer_new_wrapped_full(
        GST_MEMORY_FLAG_READONLY,
        padded.data(),
        padded.size(),
        0,
        padded.size(),
        nullptr,
        nullptr);
    assert(buffer != nullptr);

    SsvAnalysisFramePool pool(3, 2, 1);
    const PreprocessTransform transform {
        3, 2, 3, 2, 1.0F, 1.0F, 0, 0, 0, 0};
    auto frame = pool.create(buffer, info, transform, {});
    const auto *first_lease = frame->view().bytes.data();
    assert(frame->view().bytes.size() == 24);
    assert(frame->view().stride == 12);
    for (std::size_t index = 0; index < 12; ++index) {
        assert(frame->view().bytes[index] == index + 1);
        assert(frame->view().bytes[12 + index] == index + 21);
    }

    const auto occupied = pool.stats();
    assert(occupied.map_count == 1);
    assert(occupied.direct_frames == 0);
    assert(occupied.staged_frames == 1);
    assert(occupied.active_maps == 0);
    assert(occupied.outstanding_staging_leases == 1);

    try {
        static_cast<void>(pool.create(buffer, info, transform, {}));
        assert(false && "exhausted staging pool accepted another frame");
    } catch (const std::runtime_error &error) {
        assert(std::string(error.what()).find("exhausted")
            != std::string::npos);
    }
    assert(pool.stats().staging_exhaustions == 1);
    assert(pool.stats().active_maps == 0);
    assert(pool.stats().outstanding_staging_leases == 1);

    frame.reset();
    assert(pool.stats().outstanding_staging_leases == 0);
    auto reused = pool.create(buffer, info, transform, {});
    assert(reused->view().bytes.data() == first_lease);
    reused.reset();

    const auto released = pool.stats();
    assert(released.map_count == 2);
    assert(released.staged_frames == 2);
    assert(released.active_maps == 0);
    assert(released.outstanding_staging_leases == 0);
    assert(released.peak_staging_leases == 1);
    gst_buffer_unref(buffer);
}

void test_multiple_memory_blocks_are_copied_by_row_into_one_lease()
{
    GstVideoInfo info;
    gst_video_info_init(&info);
    assert(gst_video_info_set_format(&info, GST_VIDEO_FORMAT_RGBA, 3, 2));

    std::array<std::uint8_t, 7> first_memory {};
    std::array<std::uint8_t, 17> second_memory {};
    for (std::size_t index = 0; index < first_memory.size(); ++index)
        first_memory[index] = static_cast<std::uint8_t>(index + 1);
    for (std::size_t index = 0; index < second_memory.size(); ++index) {
        second_memory[index] = static_cast<std::uint8_t>(
            first_memory.size() + index + 1);
    }

    GstBuffer *buffer = gst_buffer_new();
    assert(buffer != nullptr);
    gst_buffer_append_memory(buffer, gst_memory_new_wrapped(
        GST_MEMORY_FLAG_READONLY,
        first_memory.data(),
        first_memory.size(),
        0,
        first_memory.size(),
        nullptr,
        nullptr));
    gst_buffer_append_memory(buffer, gst_memory_new_wrapped(
        GST_MEMORY_FLAG_READONLY,
        second_memory.data(),
        second_memory.size(),
        0,
        second_memory.size(),
        nullptr,
        nullptr));
    assert(gst_buffer_n_memory(buffer) == 2);

    SsvAnalysisFramePool pool(3, 2, 1);
    auto frame = pool.create(
        buffer,
        info,
        {3, 2, 3, 2, 1.0F, 1.0F, 0, 0, 0, 0},
        {});
    for (std::size_t index = 0; index < frame->view().bytes.size(); ++index)
        assert(frame->view().bytes[index] == index + 1);
    assert(pool.stats().map_count == 1);
    assert(pool.stats().staged_frames == 1);
    assert(pool.stats().active_maps == 0);
    assert(pool.stats().outstanding_staging_leases == 1);

    frame.reset();
    assert(pool.stats().outstanding_staging_leases == 0);
    gst_buffer_unref(buffer);
}

void test_rejected_detection_publish_releases_its_analysis_frame()
{
    auto source = std::make_shared<SsvSourceMeta>("camera-01");
    SsvTimelineCursor timeline(source);
    const auto generation = timeline.on_segment({0, 0, 0, 1.0}).generation;
    SsvAnalysisFramePool pool(3, 2, 1);

    auto wrong_generation_frame = make_direct_frame(
        pool, {GST_SECOND, GST_SECOND / 15, generation + 1});
    std::weak_ptr<const SsvAnalysisFrame> wrong_generation_lease =
        wrong_generation_frame;
    SsvDetectionFrame wrong_generation;
    wrong_generation.source_id = "camera-01";
    wrong_generation.timing = wrong_generation_frame->timing();
    wrong_generation.analysis_frame = std::move(wrong_generation_frame);
    assert(source->publish_detection(std::move(wrong_generation))
        == SsvMetaResult::WrongGeneration);
    assert(wrong_generation_lease.expired());
    assert(pool.stats().active_maps == 0);

    auto accepted_frame = make_direct_frame(
        pool, {2 * GST_SECOND, GST_SECOND / 15, generation});
    std::weak_ptr<const SsvAnalysisFrame> accepted_lease = accepted_frame;
    SsvDetectionFrame accepted;
    accepted.source_id = "camera-01";
    accepted.timing = accepted_frame->timing();
    accepted.analysis_frame = std::move(accepted_frame);
    assert(source->publish_detection(std::move(accepted))
        == SsvMetaResult::Published);
    assert(!accepted_lease.expired());

    auto occupied_frame = make_direct_frame(
        pool, {3 * GST_SECOND, GST_SECOND / 15, generation});
    std::weak_ptr<const SsvAnalysisFrame> occupied_lease = occupied_frame;
    SsvDetectionFrame occupied;
    occupied.source_id = "camera-01";
    occupied.timing = occupied_frame->timing();
    occupied.analysis_frame = std::move(occupied_frame);
    assert(source->publish_detection(std::move(occupied))
        == SsvMetaResult::Occupied);
    assert(occupied_lease.expired());

    auto consumed = source->consume_detection();
    assert(consumed.result == SsvMetaResult::Consumed);
    consumed.frame.reset();
    assert(accepted_lease.expired());
    assert(pool.stats().active_maps == 0);
    assert(pool.stats().outstanding_staging_leases == 0);
}

void test_tracked_snapshot_and_generation_reset_release_analysis_frames()
{
    auto source = std::make_shared<SsvSourceMeta>("camera-01");
    SsvTimelineCursor timeline(source);
    const auto generation = timeline.on_segment({0, 0, 0, 1.0}).generation;
    SsvAnalysisFramePool pool(3, 2, 1);

    auto tracked_frame = make_direct_frame(
        pool, {GST_SECOND, GST_SECOND / 15, generation});
    std::weak_ptr<const SsvAnalysisFrame> tracked_lease = tracked_frame;
    SsvDetectionFrame observation;
    observation.frame_id = 8;
    observation.source_id = "camera-01";
    observation.timing = tracked_frame->timing();
    observation.analysis_frame = std::move(tracked_frame);
    assert(source->publish_tracked(std::move(observation), {})
        == SsvMetaResult::Published);
    assert(tracked_lease.expired());
    assert(pool.stats().active_maps == 0);

    auto snapshot = source->latest_tracked_at_or_before(GST_SECOND);
    assert(snapshot != nullptr);
    assert(snapshot->frame_id == 8);
    assert(snapshot->objects.empty());

    auto detection_frame = make_direct_frame(
        pool, {2 * GST_SECOND, GST_SECOND / 15, generation});
    std::weak_ptr<const SsvAnalysisFrame> detection_lease = detection_frame;
    SsvDetectionFrame detection;
    detection.source_id = "camera-01";
    detection.timing = detection_frame->timing();
    detection.analysis_frame = std::move(detection_frame);
    assert(source->publish_detection(std::move(detection))
        == SsvMetaResult::Published);
    assert(!detection_lease.expired());

    const auto reset = timeline.on_lifecycle_reset();
    assert(reset.generation == generation + 1);
    assert(detection_lease.expired());
    assert(pool.stats().active_maps == 0);
    assert(pool.stats().outstanding_staging_leases == 0);
    assert(snapshot->frame_id == 8);
}

void test_map_failure_releases_active_map_and_staging_lease()
{
    GstVideoInfo info;
    gst_video_info_init(&info);
    assert(gst_video_info_set_format(&info, GST_VIDEO_FORMAT_RGBA, 3, 2));
    info.stride[0] = 16;
    info.size = 32;

    auto *allocator = GST_ALLOCATOR(g_object_new(
        test_failure_allocator_get_type(), nullptr));
    assert(allocator != nullptr);
    GstBuffer *buffer = gst_buffer_new();
    assert(buffer != nullptr);
    gst_buffer_append_memory(
        buffer, gst_allocator_alloc(allocator, 32, nullptr));

    SsvAnalysisFramePool pool(3, 2, 1);
    try {
        static_cast<void>(pool.create(
            buffer,
            info,
            {3, 2, 3, 2, 1.0F, 1.0F, 0, 0, 0, 0},
            {}));
        assert(false && "non-mappable memory was accepted");
    } catch (const std::runtime_error &error) {
        assert(std::string(error.what()).find("failed to map")
            != std::string::npos);
    }

    const auto released = pool.stats();
    assert(released.map_count == 1);
    assert(released.active_maps == 0);
    assert(released.outstanding_staging_leases == 0);
    gst_buffer_unref(buffer);
    gst_object_unref(allocator);
}

} // namespace

int main(int argc, char **argv)
{
    gst_init(&argc, &argv);
    test_direct_frame_maps_once_and_releases_after_shared_consumers();
    test_padded_rows_use_and_reuse_the_bounded_staging_pool();
    test_multiple_memory_blocks_are_copied_by_row_into_one_lease();
    test_rejected_detection_publish_releases_its_analysis_frame();
    test_tracked_snapshot_and_generation_reset_release_analysis_frames();
    test_map_failure_releases_active_map_and_staging_lease();
    return 0;
}
