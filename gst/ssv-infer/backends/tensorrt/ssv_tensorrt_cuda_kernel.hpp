#pragma once

#include <cstddef>

namespace ssv::infer {

struct SsvPreprocessPlan;

[[nodiscard]] bool ssv_tensorrt_cuda_preprocess_available() noexcept;

void ssv_tensorrt_launch_rgba_to_float_nchw(
    void *output,
    const void *rgba,
    std::size_t rgba_stride,
    const SsvPreprocessPlan &plan,
    void *stream);

} // namespace ssv::infer
