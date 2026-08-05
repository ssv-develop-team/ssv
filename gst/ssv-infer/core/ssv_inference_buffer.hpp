#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace ssv::infer {

class SsvInferenceBuffer {
public:
    SsvInferenceBuffer() = default;
    SsvInferenceBuffer(void *data, std::size_t size, std::size_t alignment);
    ~SsvInferenceBuffer();

    SsvInferenceBuffer(SsvInferenceBuffer &&other) noexcept;
    SsvInferenceBuffer &operator=(SsvInferenceBuffer &&other) noexcept;

    SsvInferenceBuffer(const SsvInferenceBuffer &) = delete;
    SsvInferenceBuffer &operator=(const SsvInferenceBuffer &) = delete;

    [[nodiscard]] std::span<std::byte> bytes() noexcept;
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept;

    template <typename T>
    [[nodiscard]] std::span<T> as_span()
    {
        if (size_ % sizeof(T) != 0
            || reinterpret_cast<std::uintptr_t>(data_) % alignof(T) != 0) {
            throw std::logic_error(
                "inference buffer does not satisfy requested element type");
        }
        return {static_cast<T *>(data_), size_ / sizeof(T)};
    }

    template <typename T>
    [[nodiscard]] std::span<const T> as_span() const
    {
        if (size_ % sizeof(T) != 0
            || reinterpret_cast<std::uintptr_t>(data_) % alignof(T) != 0) {
            throw std::logic_error(
                "inference buffer does not satisfy requested element type");
        }
        return {static_cast<const T *>(data_), size_ / sizeof(T)};
    }

private:
    void reset() noexcept;

    void *data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t alignment_ = 0;
};

class SsvInferenceBufferAllocator {
public:
    virtual ~SsvInferenceBufferAllocator() = default;

    [[nodiscard]] virtual SsvInferenceBuffer allocate(
        std::size_t bytes,
        std::size_t alignment) = 0;
};

class SsvDefaultInferenceBufferAllocator final
    : public SsvInferenceBufferAllocator {
public:
    [[nodiscard]] SsvInferenceBuffer allocate(
        std::size_t bytes,
        std::size_t alignment) override;
};

} // namespace ssv::infer
