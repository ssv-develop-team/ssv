#include "ssv_backend_factory.hpp"

#include <stdexcept>

namespace ssv::infer {

std::unique_ptr<InferenceBackend> create_backend(const InferenceConfig &config)
{
    RuntimeKind runtime = resolve_runtime(config.runtime, config.model_path);
    if (runtime == RuntimeKind::OnnxRuntime)
        return create_onnxruntime_backend();
    if (runtime == RuntimeKind::TensorRt) {
        if (config.device == DeviceKind::Cpu)
            throw std::invalid_argument("runtime=tensorrt does not support device=cpu");
        return create_tensorrt_backend();
    }
    throw std::invalid_argument("unsupported inference runtime");
}

} // namespace ssv::infer
