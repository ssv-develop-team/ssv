#pragma once

#include "ssv_inference_backend.hpp"

#include <onnxruntime_cxx_api.h>

#include <memory>

namespace ssv::infer {

class OnnxRuntimeBackend final : public InferenceBackend {
public:
    OnnxRuntimeBackend();
    ~OnnxRuntimeBackend() override = default;

    BackendInfo info() const override;
    ModelMetadata load(const InferenceConfig &config) override;
    std::vector<Tensor> infer(std::span<const Tensor> inputs) override;

private:
    Ort::Env env_;
    std::unique_ptr<Ort::Session> session_;
    Ort::MemoryInfo memory_info_;
    BackendInfo info_;
    std::vector<std::string> input_names_;
    std::vector<std::string> output_names_;
    ModelMetadata metadata_;
};

} // namespace ssv::infer
