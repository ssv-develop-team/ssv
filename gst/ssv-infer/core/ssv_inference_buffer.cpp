#include "core/ssv_inference_buffer.hpp"

#include <algorithm>
#include <cstddef>
#include <new>
#include <utility>

namespace ssv::infer {

SsvInferenceBuffer::SsvInferenceBuffer(
    void *data,
    std::size_t size,
    std::size_t alignment)
    : data_(data)
    , size_(size)
    , alignment_(alignment)
{
}

SsvInferenceBuffer::~SsvInferenceBuffer()
{
    reset();
}

SsvInferenceBuffer::SsvInferenceBuffer(
    SsvInferenceBuffer &&other) noexcept
    : data_(std::exchange(other.data_, nullptr))
    , size_(std::exchange(other.size_, 0))
    , alignment_(std::exchange(other.alignment_, 0))
{
}

SsvInferenceBuffer &SsvInferenceBuffer::operator=(
    SsvInferenceBuffer &&other) noexcept
{
    if (this == &other)
        return *this;
    reset();
    data_ = std::exchange(other.data_, nullptr);
    size_ = std::exchange(other.size_, 0);
    alignment_ = std::exchange(other.alignment_, 0);
    return *this;
}

std::span<std::byte> SsvInferenceBuffer::bytes() noexcept
{
    return {static_cast<std::byte *>(data_), size_};
}

std::span<const std::byte> SsvInferenceBuffer::bytes() const noexcept
{
    return {static_cast<const std::byte *>(data_), size_};
}

void SsvInferenceBuffer::reset() noexcept
{
    if (data_ != nullptr)
        ::operator delete(data_, std::align_val_t(alignment_));
    data_ = nullptr;
    size_ = 0;
    alignment_ = 0;
}

SsvInferenceBuffer SsvDefaultInferenceBufferAllocator::allocate(
    std::size_t bytes,
    std::size_t alignment)
{
    if (bytes == 0)
        throw std::invalid_argument("inference buffer size must be positive");
    const std::size_t actual_alignment =
        std::max(alignment, alignof(std::max_align_t));
    void *data = ::operator new(
        bytes, std::align_val_t(actual_alignment));
    return {data, bytes, actual_alignment};
}

} // namespace ssv::infer
