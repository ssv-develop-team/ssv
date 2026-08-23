#pragma once

#include "core/ssv_inference_buffer.hpp"
#include "core/ssv_inference_config.hpp"
#include "core/ssv_tensor.hpp"
#include "model/ssv_image_preprocessor.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>

namespace ssv::infer {

struct SsvBackendRunTimings {
    // A value is present only when the backend can attribute the whole stage.
    // H2D/D2H are engaged with zero for providers that execute on host memory.
    std::optional<std::uint64_t> h2d_us;
    std::optional<std::uint64_t> preprocess_us;
    std::optional<std::uint64_t> execution_us;
    std::optional<std::uint64_t> d2h_us;
    std::optional<std::uint64_t> unattributed_us;
};

struct SsvBackendRunResult {
    // Output views remain owned by the backend and are borrowed by the
    // engine until the next backend call or backend shutdown.
    std::span<const SsvFloatTensorView> outputs;
    SsvBackendRunTimings timings;
};

struct SsvHardwarePreprocessInput {
    // All views are borrowed until infer_hardware returns. Backends must not
    // retain the frame, plan, or CPU fallback view after that call.
    const SsvRgbaFrameView &rgba;
    const SsvPreprocessPlan &plan;
    const SsvFloatTensorView &cpu_input;
};

class InferenceBackend {
public:
    virtual ~InferenceBackend() = default;
    virtual BackendInfo info() const = 0;
    virtual ModelMetadata load(
        const InferenceConfig &config,
        SsvInferenceBufferAllocator &allocator) = 0;
    // Hardware preprocessing is optional. The engine treats a capability
    // query failure as unavailable in auto mode and as a startup error for
    // an explicit cuda request.
    virtual bool supports_hardware_preprocess(
        const SsvPreprocessPlan &) const
    {
        return false;
    }
    // The engine calls warmup only after validating the loaded model contract.
    // The input view is borrowed for the duration of this call.
    virtual void warmup(const SsvFloatTensorView &) {}
    // The input view stays borrowed until infer returns. Backends must observe
    // the per-call token so service shutdown cannot outlive that borrowed view.
    virtual SsvBackendRunResult infer(
        const SsvFloatTensorView &input,
        std::stop_token stop_token) = 0;
    // The default adapter preserves the CPU backend contract. A hardware
    // implementation overrides this method and may ignore cpu_input.
    virtual SsvBackendRunResult infer_hardware(
        const SsvHardwarePreprocessInput &input,
        std::stop_token stop_token)
    {
        return infer(input.cpu_input, stop_token);
    }
};

std::unique_ptr<InferenceBackend> create_onnxruntime_backend();
std::unique_ptr<InferenceBackend> create_tensorrt_backend();

} // namespace ssv::infer
