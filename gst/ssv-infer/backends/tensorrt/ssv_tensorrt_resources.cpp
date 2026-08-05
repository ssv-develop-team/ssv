#include "backends/tensorrt/ssv_tensorrt_resources.hpp"

#include <stdexcept>
#include <utility>

namespace ssv::infer {

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
    return device;
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
