#include "core/ssv_inference_backend.hpp"

#include <stdexcept>

namespace ssv::infer {

namespace {

class TensorRtBackendStub final : public InferenceBackend {
public:
    BackendInfo info() const override
    {
        BackendInfo info;
        info.runtime = TensorRtEngineBackendInfo {};
        return info;
    }

    bool supports_hardware_preprocess(
        const SsvPreprocessPlan &) const override
    {
        return false;
    }

    SsvBackendRunResult infer_hardware(
        const SsvHardwarePreprocessInput &,
        std::stop_token) override
    {
        throw std::runtime_error(
            "TensorRT backend is not built; hardware preprocessing is unavailable");
    }

    ModelMetadata load(
        const InferenceConfig &,
        SsvInferenceBufferAllocator &) override
    {
        throw std::runtime_error(
            "TensorRT backend is not built; set SSV_TENSORRT_MODE=enabled and rerun ./ssv build on a host with TensorRT SDK");
    }

    SsvBackendRunResult infer(
        const SsvFloatTensorView &,
        std::stop_token) override
    {
        throw std::runtime_error("TensorRT backend is not built");
    }
};

} // namespace

std::unique_ptr<InferenceBackend> create_tensorrt_backend()
{
    return std::make_unique<TensorRtBackendStub>();
}

} // namespace ssv::infer
