#include "ssv_inference_backend.hpp"

#include <stdexcept>

namespace ssv::infer {

namespace {

class TensorRtBackendStub final : public InferenceBackend {
public:
    BackendInfo info() const override
    {
        BackendInfo info;
        info.runtime = RuntimeKind::TensorRt;
        info.provider_name = "TensorRT unavailable";
        return info;
    }

    ModelMetadata load(const InferenceConfig &) override
    {
        throw std::runtime_error(
            "TensorRT backend is not built; rebuild with -Dtensorrt=enabled on a host with TensorRT SDK");
    }

    std::vector<Tensor> infer(std::span<const Tensor>) override
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
