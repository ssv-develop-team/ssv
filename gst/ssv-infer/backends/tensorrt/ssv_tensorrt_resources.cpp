#include "backends/tensorrt/ssv_tensorrt_resources.hpp"

#include "backends/tensorrt/ssv_tensorrt_cuda_kernel.hpp"

#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

namespace ssv::infer {
namespace {

std::optional<std::uint64_t> elapsed_event_us(
    SsvTensorRtCudaApi &cuda,
    void *start,
    void *end) noexcept
{
    try {
        const float milliseconds = cuda.elapsed_event_ms(start, end);
        if (!std::isfinite(milliseconds) || milliseconds < 0.0F)
            return std::nullopt;
        const double microseconds = static_cast<double>(milliseconds) * 1'000.0;
        if (microseconds
            > static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
            return std::nullopt;
        }
        return static_cast<std::uint64_t>(microseconds);
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace

SsvTensorRtExecutionResources::SsvTensorRtExecutionResources(
    std::unique_ptr<SsvTensorRtCudaApi> cuda)
    : cuda_(std::move(cuda))
{
    if (!cuda_)
        throw std::invalid_argument("TensorRT CUDA API must not be null");
}

SsvTensorRtExecutionResources::~SsvTensorRtExecutionResources()
{
    reset();
}

SsvTensorRtCudaDeviceInfo SsvTensorRtExecutionResources::start(int device_id)
{
    if (stream_ != nullptr)
        throw std::logic_error("TensorRT execution resources already started");
    const auto device = cuda_->select_device(device_id);
    stream_ = cuda_->create_stream();
    if (stream_ == nullptr)
        throw std::runtime_error("CUDA stream creation returned null");
    device_id_ = device_id;
    if (cuda_->timing_supported()) {
        try {
            timing_start_ = cuda_->create_event();
            timing_preprocess_start_ = cuda_->create_event();
            timing_execution_start_ = cuda_->create_event();
            timing_execution_end_ = cuda_->create_event();
            timing_output_end_ = cuda_->create_event();
            timing_enabled_ = timing_start_ != nullptr
                && timing_preprocess_start_ != nullptr
                && timing_execution_start_ != nullptr
                && timing_execution_end_ != nullptr
                && timing_output_end_ != nullptr;
            if (!timing_enabled_)
                destroy_timing_events();
        } catch (...) {
            destroy_timing_events();
            timing_enabled_ = false;
        }
    }
    return device;
}

bool SsvTensorRtExecutionResources::prepare_hardware_preprocess(
    int width,
    int height) noexcept
{
    if (stream_ == nullptr || device_id_ < 0
        || width <= 0 || height <= 0
        || !cuda_->supports_hardware_preprocess()) {
        return false;
    }

    const auto width_value = static_cast<std::size_t>(width);
    const auto height_value = static_cast<std::size_t>(height);
    if (width_value > std::numeric_limits<std::size_t>::max() / 4U
        || height_value
            > std::numeric_limits<std::size_t>::max()
                / (width_value * 4U)) {
        return false;
    }
    const auto stride = width_value * 4U;
    const auto bytes = stride * height_value;
    if (rgba_scratch_ != nullptr) {
        return rgba_scratch_stride_ == stride
            && rgba_scratch_bytes_ == bytes;
    }

    try {
        cuda_->activate_device(device_id_);
        void *scratch = cuda_->allocate_device(bytes);
        if (scratch == nullptr)
            return false;
        rgba_scratch_ = scratch;
        rgba_scratch_stride_ = stride;
        rgba_scratch_bytes_ = bytes;
        return true;
    } catch (...) {
        return false;
    }
}

void SsvTensorRtExecutionResources::preprocess_rgba_to_float_nchw(
    std::size_t output_buffer_index,
    const SsvRgbaFrameView &rgba,
    const SsvPreprocessPlan &plan)
{
    if (!hardware_preprocess_available()) {
        throw std::runtime_error(
            "TensorRT hardware preprocessing is not available");
    }
    if (stream_ == nullptr || device_id_ < 0)
        throw std::logic_error("TensorRT execution resources are not started");
    if (output_buffer_index >= device_buffers_.size())
        throw std::out_of_range("TensorRT input buffer index is out of range");
    if (plan.canvas_width <= 0 || plan.canvas_height <= 0
        || rgba.width != plan.canvas_width
        || rgba.height != plan.canvas_height) {
        throw std::invalid_argument(
            "RGBA canvas dimensions do not match hardware preprocess plan");
    }
    const auto row_bytes = static_cast<std::size_t>(rgba.width) * 4U;
    if (rgba.stride < row_bytes)
        throw std::invalid_argument("RGBA canvas stride is smaller than one row");
    const auto last_row = static_cast<std::size_t>(rgba.height - 1)
        * rgba.stride;
    if (last_row > std::numeric_limits<std::size_t>::max() - row_bytes
        || last_row + row_bytes > rgba.bytes.size()) {
        throw std::invalid_argument("RGBA canvas bytes do not cover its layout");
    }
    const auto pixel_count = static_cast<std::size_t>(plan.canvas_width)
        * static_cast<std::size_t>(plan.canvas_height);
    if (pixel_count > std::numeric_limits<std::size_t>::max() / 3U
        || pixel_count * 3U
            > std::numeric_limits<std::size_t>::max() / sizeof(float)) {
        throw std::invalid_argument("TensorRT input size overflows size_t");
    }
    require_buffer_size(
        output_buffer_index, pixel_count * 3U * sizeof(float));

    cuda_->copy_host_to_device_2d(
        rgba_scratch_,
        rgba_scratch_stride_,
        rgba.bytes.data(),
        rgba.stride,
        row_bytes,
        static_cast<std::size_t>(rgba.height),
        stream_);
    mark_preprocess_start();
    cuda_->launch_rgba_to_float_nchw(
        device_buffers_[output_buffer_index],
        rgba_scratch_,
        rgba_scratch_stride_,
        plan,
        stream_);
}

void SsvTensorRtExecutionResources::allocate(
    std::span<const std::size_t> buffer_sizes)
{
    if (stream_ == nullptr)
        throw std::logic_error("TensorRT execution resources are not started");
    if (!device_buffers_.empty()) {
        throw std::logic_error("TensorRT device buffers are already allocated");
    }
    if (buffer_sizes.empty())
        throw std::invalid_argument("TensorRT requires at least one binding");

    cuda_->activate_device(device_id_);
    device_buffers_.reserve(buffer_sizes.size());
    buffer_sizes_.reserve(buffer_sizes.size());
    try {
        for (const std::size_t bytes : buffer_sizes) {
            if (bytes == 0) {
                throw std::invalid_argument(
                    "TensorRT device buffer size must be positive");
            }
            void *device = cuda_->allocate_device(bytes);
            if (device == nullptr) {
                throw std::runtime_error(
                    "CUDA device allocation returned null");
            }
            device_buffers_.push_back(device);
            buffer_sizes_.push_back(bytes);
        }
    } catch (...) {
        reset();
        throw;
    }
}

void *SsvTensorRtExecutionResources::activate_stream()
{
    if (stream_ == nullptr || device_id_ < 0)
        throw std::logic_error("TensorRT execution resources are not started");
    cuda_->activate_device(device_id_);
    return stream_;
}

std::span<void *const>
SsvTensorRtExecutionResources::device_buffers() const noexcept
{
    return {device_buffers_.data(), device_buffers_.size()};
}

void SsvTensorRtExecutionResources::copy_input(
    std::size_t buffer_index, std::span<const float> input)
{
    if (input.size() > std::numeric_limits<std::size_t>::max() / sizeof(float))
        throw std::invalid_argument("TensorRT input size overflows size_t");
    const auto bytes = input.size() * sizeof(float);
    require_buffer_size(buffer_index, bytes);
    cuda_->copy_host_to_device(
        device_buffers_[buffer_index], input.data(), bytes, stream_);
}

void SsvTensorRtExecutionResources::copy_input(
    std::size_t buffer_index, std::span<const std::uint8_t> input)
{
    require_buffer_size(buffer_index, input.size());
    cuda_->copy_host_to_device(
        device_buffers_[buffer_index], input.data(), input.size(), stream_);
}

void SsvTensorRtExecutionResources::copy_output(
    std::size_t buffer_index, std::span<std::byte> output)
{
    require_buffer_size(buffer_index, output.size());
    cuda_->copy_device_to_host(
        output.data(), device_buffers_[buffer_index], output.size(), stream_);
}

void SsvTensorRtExecutionResources::synchronize()
{
    if (stream_ == nullptr)
        throw std::logic_error("TensorRT execution resources are not started");
    cuda_->synchronize(stream_);
}

void SsvTensorRtExecutionResources::begin_run_timing() noexcept
{
    if (!timing_enabled_)
        return;
    timing_run_active_ = false;
    timing_preprocess_recorded_ = false;
    try {
        cuda_->record_event(timing_start_, stream_);
        timing_run_active_ = true;
    } catch (...) {
        disable_timing();
    }
}

void SsvTensorRtExecutionResources::mark_preprocess_start() noexcept
{
    if (!timing_enabled_ || !timing_run_active_)
        return;
    try {
        cuda_->record_event(timing_preprocess_start_, stream_);
        timing_preprocess_recorded_ = true;
    } catch (...) {
        disable_timing();
    }
}

void SsvTensorRtExecutionResources::mark_execution_start() noexcept
{
    if (!timing_enabled_ || !timing_run_active_)
        return;
    try {
        cuda_->record_event(timing_execution_start_, stream_);
    } catch (...) {
        disable_timing();
    }
}

void SsvTensorRtExecutionResources::mark_execution_end() noexcept
{
    if (!timing_enabled_ || !timing_run_active_)
        return;
    try {
        cuda_->record_event(timing_execution_end_, stream_);
    } catch (...) {
        disable_timing();
    }
}

void SsvTensorRtExecutionResources::mark_output_end() noexcept
{
    if (!timing_enabled_ || !timing_run_active_)
        return;
    try {
        cuda_->record_event(timing_output_end_, stream_);
    } catch (...) {
        disable_timing();
    }
}

SsvBackendRunTimings SsvTensorRtExecutionResources::finish_run_timing() noexcept
{
    if (!timing_run_active_)
        return {};
    timing_run_active_ = false;
    const bool preprocess_recorded = timing_preprocess_recorded_;
    timing_preprocess_recorded_ = false;
    if (!timing_enabled_)
        return {};
    auto h2d = preprocess_recorded
        ? elapsed_event_us(*cuda_, timing_start_, timing_preprocess_start_)
        : elapsed_event_us(*cuda_, timing_start_, timing_execution_start_);
    auto preprocess = preprocess_recorded
        ? elapsed_event_us(
            *cuda_, timing_preprocess_start_, timing_execution_start_)
        : std::nullopt;
    return {
        .h2d_us = std::move(h2d),
        .preprocess_us = std::move(preprocess),
        .execution_us = elapsed_event_us(
            *cuda_, timing_execution_start_, timing_execution_end_),
        .d2h_us = elapsed_event_us(
            *cuda_, timing_execution_end_, timing_output_end_),
        .unattributed_us = std::nullopt,
    };
}

void SsvTensorRtExecutionResources::reset() noexcept
{
    if (device_id_ >= 0) {
        try {
            cuda_->activate_device(device_id_);
        } catch (...) {
            // Destructors cannot report CUDA teardown failures. Continue with
            // best-effort release rather than leaking every remaining handle.
        }
    }
    destroy_timing_events();
    timing_enabled_ = false;
    timing_run_active_ = false;
    timing_preprocess_recorded_ = false;
    if (rgba_scratch_ != nullptr) {
        cuda_->free_device(rgba_scratch_);
        rgba_scratch_ = nullptr;
    }
    rgba_scratch_bytes_ = 0;
    rgba_scratch_stride_ = 0;
    for (auto current = device_buffers_.rbegin();
         current != device_buffers_.rend();
         ++current) {
        cuda_->free_device(*current);
    }
    device_buffers_.clear();
    buffer_sizes_.clear();
    if (stream_ != nullptr) {
        cuda_->destroy_stream(stream_);
        stream_ = nullptr;
    }
    device_id_ = -1;
}

void SsvTensorRtExecutionResources::disable_timing() noexcept
{
    timing_enabled_ = false;
    timing_run_active_ = false;
    timing_preprocess_recorded_ = false;
}

void SsvTensorRtExecutionResources::destroy_timing_events() noexcept
{
    if (timing_start_ != nullptr)
        cuda_->destroy_event(timing_start_);
    if (timing_preprocess_start_ != nullptr)
        cuda_->destroy_event(timing_preprocess_start_);
    if (timing_execution_start_ != nullptr)
        cuda_->destroy_event(timing_execution_start_);
    if (timing_execution_end_ != nullptr)
        cuda_->destroy_event(timing_execution_end_);
    if (timing_output_end_ != nullptr)
        cuda_->destroy_event(timing_output_end_);
    timing_start_ = nullptr;
    timing_preprocess_start_ = nullptr;
    timing_execution_start_ = nullptr;
    timing_execution_end_ = nullptr;
    timing_output_end_ = nullptr;
}

void SsvTensorRtExecutionResources::require_buffer_size(
    std::size_t buffer_index, std::size_t bytes) const
{
    if (stream_ == nullptr)
        throw std::logic_error("TensorRT execution resources are not started");
    if (buffer_index >= device_buffers_.size())
        throw std::out_of_range("TensorRT device buffer index is out of range");
    if (buffer_sizes_[buffer_index] != bytes) {
        throw std::invalid_argument(
            "TensorRT transfer size does not match fixed device buffer");
    }
}

} // namespace ssv::infer
