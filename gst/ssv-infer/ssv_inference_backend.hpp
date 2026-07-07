#pragma once

#include "ssv_inference_config.hpp"

#include <memory>
#include <span>

namespace ssv::infer {

class InferenceBackend {
public:
    virtual ~InferenceBackend() = default;
    virtual BackendInfo info() const = 0;
    virtual ModelMetadata load(const InferenceConfig &config) = 0;
    virtual std::vector<Tensor> infer(std::span<const Tensor> inputs) = 0;
};

std::unique_ptr<InferenceBackend> create_onnxruntime_backend();
std::unique_ptr<InferenceBackend> create_tensorrt_backend();

} // namespace ssv::infer
