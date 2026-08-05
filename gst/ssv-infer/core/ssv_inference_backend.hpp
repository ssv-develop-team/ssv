#pragma once

#include "core/ssv_inference_buffer.hpp"
#include "core/ssv_inference_config.hpp"

#include <memory>
#include <span>
#include <stop_token>

namespace ssv::infer {

class InferenceBackend {
public:
    virtual ~InferenceBackend() = default;
    virtual BackendInfo info() const = 0;
    virtual ModelMetadata load(
        const InferenceConfig &config,
        SsvInferenceBufferAllocator &allocator) = 0;
    // The engine calls warmup only after validating the loaded model contract.
    virtual void warmup() {}
    // The input view stays borrowed until infer returns. Backends must observe
    // the per-call token so service shutdown cannot outlive that borrowed view.
    virtual std::span<const SsvFloatTensorView> infer(
        const SsvUint8TensorView &input,
        std::stop_token stop_token) = 0;
};

std::unique_ptr<InferenceBackend> create_onnxruntime_backend();
std::unique_ptr<InferenceBackend> create_tensorrt_backend();

} // namespace ssv::infer
