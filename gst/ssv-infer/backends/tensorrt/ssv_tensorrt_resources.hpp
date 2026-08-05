#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace ssv::infer {

struct SsvTensorRtCudaDeviceInfo {
    int cuda_runtime_version = 0;
    int compute_capability_major = -1;
    int compute_capability_minor = -1;
};

class SsvTensorRtCudaApi {
public:
    virtual ~SsvTensorRtCudaApi() = default;

    [[nodiscard]] virtual SsvTensorRtCudaDeviceInfo select_device(int device_id)
        = 0;
    virtual void activate_device(int device_id) = 0;
    [[nodiscard]] virtual void *create_stream() = 0;
    virtual void destroy_stream(void *stream) noexcept = 0;
    [[nodiscard]] virtual void *allocate_device(std::size_t bytes) = 0;
    virtual void free_device(void *device) noexcept = 0;
    virtual void copy_host_to_device(
        void *device, const void *host, std::size_t bytes, void *stream)
        = 0;
    virtual void copy_device_to_host(
        void *host, const void *device, std::size_t bytes, void *stream)
        = 0;
    virtual void synchronize(void *stream) = 0;
};

class SsvTensorRtExecutionResources final {
public:
    explicit SsvTensorRtExecutionResources(
        std::unique_ptr<SsvTensorRtCudaApi> cuda);
    ~SsvTensorRtExecutionResources();

    SsvTensorRtExecutionResources(const SsvTensorRtExecutionResources &)
        = delete;
    SsvTensorRtExecutionResources &operator=(
        const SsvTensorRtExecutionResources &)
        = delete;

    [[nodiscard]] SsvTensorRtCudaDeviceInfo start(int device_id);
    void allocate(std::span<const std::size_t> buffer_sizes);

    // Makes the resource device current for the calling thread before the
    // reusable stream is passed to TensorRT.
    [[nodiscard]] void *activate_stream();
    [[nodiscard]] std::span<void *const> device_buffers() const noexcept;

    void copy_input(
        std::size_t buffer_index, std::span<const std::uint8_t> input);
    void copy_output(std::size_t buffer_index, std::span<std::byte> output);
    void synchronize();

private:
    void reset() noexcept;
    void require_buffer_size(std::size_t buffer_index, std::size_t bytes) const;

    std::unique_ptr<SsvTensorRtCudaApi> cuda_;
    int device_id_ = -1;
    void *stream_ = nullptr;
    std::vector<void *> device_buffers_;
    std::vector<std::size_t> buffer_sizes_;
};

} // namespace ssv::infer
