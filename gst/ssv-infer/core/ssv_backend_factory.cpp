#include "core/ssv_backend_factory.hpp"

#include <stdexcept>

namespace ssv::infer {

std::unique_ptr<InferenceBackend> create_backend(const InferenceConfig &config)
{
    if (config.runtime == RuntimeKind::OnnxRuntime)
        return create_onnxruntime_backend();
    if (config.runtime == RuntimeKind::TensorRtEngine)
        return create_tensorrt_backend();
    throw std::invalid_argument("unsupported inference runtime");
}

} // namespace ssv::infer
