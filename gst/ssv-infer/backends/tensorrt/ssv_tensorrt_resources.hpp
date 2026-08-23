#pragma once

#include "core/ssv_inference_backend.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
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
    virtual void copy_host_to_device_2d(
        void *device,
        std::size_t device_pitch,
        const void *host,
        std::size_t host_pitch,
        std::size_t width_bytes,
        std::size_t height,
        void *stream)
    {
        if (device_pitch != width_bytes || host_pitch != width_bytes) {
            throw std::invalid_argument(
                "CUDA API does not support strided host-to-device copies");
        }
        if (height != 0
            && width_bytes > std::numeric_limits<std::size_t>::max() / height) {
            throw std::invalid_argument(
                "CUDA host-to-device copy size overflows size_t");
        }
        copy_host_to_device(
            device, host, width_bytes * height, stream);
    }
    virtual void copy_device_to_host(
        void *host, const void *device, std::size_t bytes, void *stream)
        = 0;
    virtual void synchronize(void *stream) = 0;

    [[nodiscard]] virtual bool supports_hardware_preprocess() const noexcept
    {
        return false;
    }
    virtual void launch_rgba_to_float_nchw(
        void *,
        const void *,
        std::size_t,
        const SsvPreprocessPlan &,
        void *)
    {
        throw std::runtime_error(
            "CUDA RGBA preprocessing is not available");
    }

    // Timing is optional: a test/fallback CUDA implementation may leave it
    // disabled without changing inference correctness.
    [[nodiscard]] virtual bool timing_supported() const noexcept
    {
        return false;
    }
    [[nodiscard]] virtual void *create_event() { return nullptr; }
    virtual void destroy_event(void *) noexcept {}
    virtual void record_event(void *, void *) {}
    [[nodiscard]] virtual float elapsed_event_ms(void *, void *)
    {
        return 0.0F;
    }
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

    // The scratch is allocated once for the model canvas and reused for every
    // frame. Returning false leaves normal TensorRT inference available.
    [[nodiscard]] bool prepare_hardware_preprocess(
        int width,
        int height) noexcept;
    [[nodiscard]] bool hardware_preprocess_available() const noexcept
    {
        return rgba_scratch_ != nullptr
            && cuda_->supports_hardware_preprocess();
    }
    void preprocess_rgba_to_float_nchw(
        std::size_t output_buffer_index,
        const SsvRgbaFrameView &rgba,
        const SsvPreprocessPlan &plan);

    void copy_input(
        std::size_t buffer_index, std::span<const float> input);
    void copy_input(
        std::size_t buffer_index, std::span<const std::uint8_t> input);
    void copy_output(std::size_t buffer_index, std::span<std::byte> output);
    void synchronize();

    [[nodiscard]] bool timing_enabled() const noexcept
    {
        return timing_enabled_;
    }
    void begin_run_timing() noexcept;
    void mark_preprocess_start() noexcept;
    void mark_execution_start() noexcept;
    void mark_execution_end() noexcept;
    void mark_output_end() noexcept;
    [[nodiscard]] SsvBackendRunTimings finish_run_timing() noexcept;

private:
    void reset() noexcept;
    void disable_timing() noexcept;
    void destroy_timing_events() noexcept;
    void require_buffer_size(std::size_t buffer_index, std::size_t bytes) const;

    std::unique_ptr<SsvTensorRtCudaApi> cuda_;
    int device_id_ = -1;
    void *stream_ = nullptr;
    std::vector<void *> device_buffers_;
    std::vector<std::size_t> buffer_sizes_;
    void *rgba_scratch_ = nullptr;
    std::size_t rgba_scratch_bytes_ = 0;
    std::size_t rgba_scratch_stride_ = 0;
    void *timing_start_ = nullptr;
    void *timing_preprocess_start_ = nullptr;
    void *timing_execution_start_ = nullptr;
    void *timing_execution_end_ = nullptr;
    void *timing_output_end_ = nullptr;
    bool timing_enabled_ = false;
    bool timing_run_active_ = false;
    bool timing_preprocess_recorded_ = false;
};

} // namespace ssv::infer
