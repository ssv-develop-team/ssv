#pragma once

#include "core/ssv_inference_backend.hpp"
#include "backends/onnxruntime/ssv_session_pool.hpp"

#include <onnxruntime_cxx_api.h>

#include <memory>

namespace ssv::infer {

struct SsvOrtSessionState;

class OnnxRuntimeBackend final : public InferenceBackend {
public:
    OnnxRuntimeBackend();
    ~OnnxRuntimeBackend() override = default;

    BackendInfo info() const override;
    ModelMetadata load(
        const InferenceConfig &config,
        SsvInferenceBufferAllocator &allocator) override;
    void warmup(const SsvFloatTensorView &input) override;
    SsvBackendRunResult infer(
        const SsvFloatTensorView &input,
        std::stop_token stop_token) override;

private:
    void terminate_run() noexcept;

    SsvRuntimeProfile profile_;
    Ort::Env env_;
    std::shared_ptr<SsvSessionPool<SsvOrtSessionState>> session_pool_;
    std::shared_ptr<SsvOrtSessionState> session_state_;
    Ort::MemoryInfo memory_info_;
    Ort::RunOptions run_options_;
    BackendInfo info_;
    std::vector<std::string> input_names_;
    std::vector<std::string> output_names_;
    std::vector<const char *> input_name_ptrs_;
    std::vector<const char *> output_name_ptrs_;
    ModelMetadata metadata_;
    std::vector<SsvInferenceBuffer> output_buffers_;
    std::vector<Ort::Value> output_values_;
    std::vector<SsvFloatTensorView> output_views_;
};

} // namespace ssv::infer
