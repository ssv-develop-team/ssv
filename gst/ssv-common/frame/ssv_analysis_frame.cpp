#include "ssv_meta.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

std::size_t checked_frame_bytes(int width, int height)
{
    const auto row_bytes = static_cast<std::size_t>(width) * 4U;
    if (row_bytes > std::numeric_limits<std::size_t>::max()
            / static_cast<std::size_t>(height)) {
        throw std::invalid_argument("analysis frame byte size overflows");
    }
    return row_bytes * static_cast<std::size_t>(height);
}

struct AnalysisFramePoolState {
    AnalysisFramePoolState(
        std::size_t staging_bytes,
        std::size_t staging_capacity)
        : staging_storage(staging_bytes)
    {
        free_staging_slots.reserve(staging_capacity);
        for (std::size_t index = staging_capacity; index > 0; --index)
            free_staging_slots.push_back(index - 1);
    }

    mutable std::mutex mutex;
    SsvAnalysisFramePoolStats stats;
    std::vector<std::uint8_t> staging_storage;
    std::vector<std::size_t> free_staging_slots;
};

struct PlaneLayout {
    std::size_t offset = 0;
    std::size_t stride = 0;
};

void validate_frame_contract(
    int model_width,
    int model_height,
    const GstVideoInfo &video_info,
    const PreprocessTransform &transform)
{
    if (GST_VIDEO_INFO_FORMAT(&video_info) != GST_VIDEO_FORMAT_RGBA
        || GST_VIDEO_INFO_WIDTH(&video_info) != model_width
        || GST_VIDEO_INFO_HEIGHT(&video_info) != model_height) {
        throw std::invalid_argument(
            "analysis frame must match the model-sized RGBA contract");
    }
    if (transform.source_width <= 0 || transform.source_height <= 0
        || transform.model_width != model_width
        || transform.model_height != model_height
        || !std::isfinite(transform.scale_x) || transform.scale_x <= 0.0F
        || !std::isfinite(transform.scale_y) || transform.scale_y <= 0.0F
        || transform.pad_left < 0 || transform.pad_top < 0
        || transform.pad_right < 0 || transform.pad_bottom < 0
        || transform.pad_right > model_width
        || transform.pad_left > model_width - transform.pad_right
        || transform.pad_bottom > model_height
        || transform.pad_top > model_height - transform.pad_bottom) {
        throw std::invalid_argument("invalid preprocess transform");
    }
}

PlaneLayout plane_layout(
    GstBuffer *buffer,
    const GstVideoInfo &video_info,
    std::size_t row_bytes)
{
    gint signed_stride = GST_VIDEO_INFO_PLANE_STRIDE(&video_info, 0);
    std::size_t offset = GST_VIDEO_INFO_PLANE_OFFSET(&video_info, 0);
    if (const auto *meta = gst_buffer_get_video_meta(buffer)) {
        if (meta->format != GST_VIDEO_FORMAT_RGBA
            || meta->width != static_cast<guint>(
                GST_VIDEO_INFO_WIDTH(&video_info))
            || meta->height != static_cast<guint>(
                GST_VIDEO_INFO_HEIGHT(&video_info))
            || meta->n_planes != 1) {
            throw std::invalid_argument(
                "RGBA video meta does not match negotiated caps");
        }
        signed_stride = meta->stride[0];
        offset = meta->offset[0];
    }
    if (signed_stride <= 0
        || static_cast<std::size_t>(signed_stride) < row_bytes) {
        throw std::invalid_argument("RGBA frame stride is invalid");
    }
    return {offset, static_cast<std::size_t>(signed_stride)};
}

} // namespace

struct SsvAnalysisFrame::Impl {
    ~Impl()
    {
        if (mapped) {
            gst_video_frame_unmap(&video_frame);
            std::lock_guard<std::mutex> lock(pool_state->mutex);
            --pool_state->stats.active_maps;
        }
        if (staging_slot) {
            std::lock_guard<std::mutex> lock(pool_state->mutex);
            pool_state->free_staging_slots.push_back(*staging_slot);
            --pool_state->stats.outstanding_staging_leases;
        }
    }

    SsvRgbaFrameView view;
    PreprocessTransform transform;
    SsvFrameTiming timing;
    std::shared_ptr<AnalysisFramePoolState> pool_state;
    GstVideoFrame video_frame {};
    bool mapped = false;
    std::optional<std::size_t> staging_slot;
};

struct SsvAnalysisFramePool::Impl {
    int model_width = 0;
    int model_height = 0;
    std::shared_ptr<AnalysisFramePoolState> state;
};

SsvAnalysisFrame::SsvAnalysisFrame(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl))
{
}

SsvAnalysisFrame::~SsvAnalysisFrame() = default;

const SsvRgbaFrameView &SsvAnalysisFrame::view() const noexcept
{
    return impl_->view;
}

const PreprocessTransform &SsvAnalysisFrame::transform() const noexcept
{
    return impl_->transform;
}

const SsvFrameTiming &SsvAnalysisFrame::timing() const noexcept
{
    return impl_->timing;
}

SsvAnalysisFramePool::SsvAnalysisFramePool(
    int model_width,
    int model_height,
    std::size_t staging_capacity)
    : impl_(std::make_unique<Impl>())
{
    if (model_width <= 0 || model_height <= 0)
        throw std::invalid_argument("analysis model dimensions must be positive");
    if (staging_capacity == 0)
        throw std::invalid_argument("analysis staging capacity must be positive");
    const auto frame_bytes = checked_frame_bytes(model_width, model_height);
    if (staging_capacity
        > std::numeric_limits<std::size_t>::max() / frame_bytes) {
        throw std::invalid_argument("analysis staging pool byte size overflows");
    }
    impl_->model_width = model_width;
    impl_->model_height = model_height;
    impl_->state = std::make_shared<AnalysisFramePoolState>(
        frame_bytes * staging_capacity, staging_capacity);
}

SsvAnalysisFramePool::~SsvAnalysisFramePool() = default;

std::shared_ptr<const SsvAnalysisFrame> SsvAnalysisFramePool::create(
    GstBuffer *buffer,
    const GstVideoInfo &video_info,
    PreprocessTransform transform,
    SsvFrameTiming timing)
{
    if (buffer == nullptr)
        throw std::invalid_argument("analysis buffer must not be null");
    validate_frame_contract(
        impl_->model_width, impl_->model_height, video_info, transform);

    const auto row_bytes =
        static_cast<std::size_t>(impl_->model_width) * 4U;
    const auto frame_bytes =
        checked_frame_bytes(impl_->model_width, impl_->model_height);
    const auto layout = plane_layout(buffer, video_info, row_bytes);
    const auto height = static_cast<std::size_t>(impl_->model_height);
    auto last_row = layout.offset;
    if (height > 1) {
        const auto row_count = height - 1;
        if (layout.stride
            > (std::numeric_limits<std::size_t>::max() - layout.offset)
                / row_count) {
            throw std::invalid_argument("RGBA frame layout overflows");
        }
        last_row += row_count * layout.stride;
    }
    if (row_bytes > std::numeric_limits<std::size_t>::max() - last_row
        || last_row + row_bytes > gst_buffer_get_size(buffer)) {
        throw std::invalid_argument("RGBA frame layout exceeds its buffer");
    }

    auto frame_impl = std::make_unique<SsvAnalysisFrame::Impl>();
    frame_impl->transform = std::move(transform);
    frame_impl->timing = timing;
    frame_impl->pool_state = impl_->state;
    const bool direct = gst_buffer_n_memory(buffer) == 1
        && layout.stride == row_bytes;
    if (!direct) {
        std::lock_guard<std::mutex> lock(impl_->state->mutex);
        if (impl_->state->free_staging_slots.empty()) {
            ++impl_->state->stats.staging_exhaustions;
            throw std::runtime_error("analysis staging pool is exhausted");
        }
        frame_impl->staging_slot =
            impl_->state->free_staging_slots.back();
        impl_->state->free_staging_slots.pop_back();
        ++impl_->state->stats.outstanding_staging_leases;
        impl_->state->stats.peak_staging_leases = std::max(
            impl_->state->stats.peak_staging_leases,
            impl_->state->stats.outstanding_staging_leases);
    }

    {
        std::lock_guard<std::mutex> lock(impl_->state->mutex);
        ++impl_->state->stats.map_count;
        ++impl_->state->stats.active_maps;
        impl_->state->stats.peak_active_maps = std::max(
            impl_->state->stats.peak_active_maps,
            impl_->state->stats.active_maps);
    }
    if (direct && !gst_video_frame_map(
            &frame_impl->video_frame,
            &video_info,
            buffer,
            GST_MAP_READ)) {
        std::lock_guard<std::mutex> lock(impl_->state->mutex);
        --impl_->state->stats.active_maps;
        throw std::runtime_error("failed to map model-sized RGBA frame");
    }

    if (direct) {
        frame_impl->mapped = true;
        const auto mapped_stride = GST_VIDEO_FRAME_PLANE_STRIDE(
            &frame_impl->video_frame, 0);
        if (mapped_stride != static_cast<int>(row_bytes)) {
            throw std::runtime_error(
                "mapped RGBA frame does not have a tight stride");
        }
        const auto *data = static_cast<const std::uint8_t *>(
            GST_VIDEO_FRAME_PLANE_DATA(&frame_impl->video_frame, 0));
        if (data == nullptr)
            throw std::runtime_error("mapped RGBA frame has no data");
        frame_impl->view = {
            std::span<const std::uint8_t>(data, frame_bytes),
            impl_->model_width,
            impl_->model_height,
            row_bytes,
        };
        std::lock_guard<std::mutex> lock(impl_->state->mutex);
        ++impl_->state->stats.direct_frames;
        return std::shared_ptr<const SsvAnalysisFrame>(
            new SsvAnalysisFrame(std::move(frame_impl)));
    }

    auto *staging_data = impl_->state->staging_storage.data()
        + *frame_impl->staging_slot * frame_bytes;
    std::size_t buffer_offset = 0;
    std::size_t copied_bytes = 0;
    const auto memory_count = gst_buffer_n_memory(buffer);
    for (guint memory_index = 0; memory_index < memory_count; ++memory_index) {
        auto *memory = gst_buffer_peek_memory(buffer, memory_index);
        GstMapInfo map_info = GST_MAP_INFO_INIT;
        if (!gst_memory_map(memory, &map_info, GST_MAP_READ)) {
            std::lock_guard<std::mutex> lock(impl_->state->mutex);
            --impl_->state->stats.active_maps;
            throw std::runtime_error("failed to map RGBA buffer memory");
        }
        if (map_info.size > 0 && map_info.data == nullptr) {
            gst_memory_unmap(memory, &map_info);
            std::lock_guard<std::mutex> lock(impl_->state->mutex);
            --impl_->state->stats.active_maps;
            throw std::runtime_error("mapped RGBA memory has no data");
        }

        if (map_info.size
            > std::numeric_limits<std::size_t>::max() - buffer_offset) {
            gst_memory_unmap(memory, &map_info);
            std::lock_guard<std::mutex> lock(impl_->state->mutex);
            --impl_->state->stats.active_maps;
            throw std::runtime_error("RGBA memory layout overflows");
        }
        const auto memory_end = buffer_offset + map_info.size;
        for (std::size_t row = 0; row < height; ++row) {
            const auto source_begin = layout.offset + row * layout.stride;
            const auto source_end = source_begin + row_bytes;
            const auto overlap_begin = std::max(source_begin, buffer_offset);
            const auto overlap_end = std::min(source_end, memory_end);
            if (overlap_begin >= overlap_end)
                continue;
            const auto bytes = overlap_end - overlap_begin;
            std::memcpy(
                staging_data + row * row_bytes
                    + (overlap_begin - source_begin),
                map_info.data + (overlap_begin - buffer_offset),
                bytes);
            copied_bytes += bytes;
        }
        gst_memory_unmap(memory, &map_info);
        buffer_offset = memory_end;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->state->mutex);
        --impl_->state->stats.active_maps;
    }
    if (copied_bytes != frame_bytes)
        throw std::runtime_error("RGBA buffer does not cover every model row");

    frame_impl->view = {
        std::span<const std::uint8_t>(staging_data, frame_bytes),
        impl_->model_width,
        impl_->model_height,
        row_bytes,
    };
    {
        std::lock_guard<std::mutex> lock(impl_->state->mutex);
        ++impl_->state->stats.staged_frames;
    }
    return std::shared_ptr<const SsvAnalysisFrame>(
        new SsvAnalysisFrame(std::move(frame_impl)));
}

SsvAnalysisFramePoolStats SsvAnalysisFramePool::stats() const noexcept
{
    std::lock_guard<std::mutex> lock(impl_->state->mutex);
    return impl_->state->stats;
}
