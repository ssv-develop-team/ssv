#include "runtime.hpp"

#include "onnx_cpu_runtime.hpp"
#include "onnx_gpu_runtime.hpp"

namespace ssv::provider {

std::unique_ptr<Runtime> create_runtime(
    const std::string& backend,
    const std::string& device,
    std::string* error)
{
    if (backend != "onnx") {
        if (error) {
            *error = "unsupported inference runtime: " + backend;
        }
        return nullptr;
    }

    if (device == "cpu") {
        return std::make_unique<OnnxCpuRuntime>();
    }
    if (device == "gpu") {
        return std::make_unique<OnnxGpuRuntime>();
    }
    if (error) {
        *error = "unsupported inference device: " + device;
    }
    return nullptr;
}

} // namespace ssv::provider
