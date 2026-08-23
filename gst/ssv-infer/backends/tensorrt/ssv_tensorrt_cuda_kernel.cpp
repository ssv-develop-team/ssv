#include "backends/tensorrt/ssv_tensorrt_cuda_kernel.hpp"

#include <stdexcept>

namespace ssv::infer {

bool ssv_tensorrt_cuda_preprocess_available() noexcept
{
    return false;
}

void ssv_tensorrt_launch_rgba_to_float_nchw(
    void *,
    const void *,
    std::size_t,
    const SsvPreprocessPlan &,
    void *)
{
    throw std::runtime_error(
        "TensorRT CUDA preprocessing kernel is not built");
}

} // namespace ssv::infer
