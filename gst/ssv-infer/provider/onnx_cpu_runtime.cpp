#include "onnx_cpu_runtime.hpp"

namespace ssv::provider {

bool OnnxCpuRuntime::configure(
    Ort::SessionOptions&,
    RuntimeStatus* status,
    std::string*) const
{
    if (status) {
        status->requested_runtime = "onnx";
        status->active_runtime = "CPUExecutionProvider";
    }
    return true;
}

} // namespace ssv::provider
