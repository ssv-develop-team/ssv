#include "onnx_gpu_runtime.hpp"

#include <climits>
#include <cerrno>
#include <cstdlib>

namespace ssv::provider {

bool read_cuda_device_id_from_env(int* device_id, std::string* error) {
    const char* raw = std::getenv("SSV_INFER_CUDA_DEVICE_ID");
    if (!raw || raw[0] == '\0') {
        *device_id = 0;
        return true;
    }

    errno = 0;
    char* end = nullptr;
    long value = std::strtol(raw, &end, 10);
    if (errno != 0 || end == raw || *end != '\0' || value < 0 || value > INT_MAX) {
        if (error) {
            *error = std::string("invalid SSV_INFER_CUDA_DEVICE_ID: ") + raw;
        }
        return false;
    }

    *device_id = static_cast<int>(value);
    return true;
}

bool OnnxGpuRuntime::configure(
    Ort::SessionOptions& options,
    RuntimeStatus* status,
    std::string* error) const
{
    int cuda_device_id = 0;
    if (!read_cuda_device_id_from_env(&cuda_device_id, error)) {
        return false;
    }

    try {
        OrtCUDAProviderOptions cuda_options{};
        cuda_options.device_id = cuda_device_id;
        options.AppendExecutionProvider_CUDA(cuda_options);
    } catch (const Ort::Exception& e) {
        if (error) {
            *error = std::string("CUDAExecutionProvider unavailable: ") + e.what();
        }
        return false;
    }

    if (status) {
        status->requested_runtime = "onnx";
        status->active_runtime = "CUDAExecutionProvider";
    }
    return true;
}

} // namespace ssv::provider
