#include "backends/tensorrt/ssv_tensorrt_cuda_kernel.hpp"

#include "model/ssv_image_preprocessor.hpp"

#include <cuda_runtime.h>

#include <cstddef>
#include <stdexcept>
#include <string>

namespace ssv::infer {
namespace {

void check_cuda(cudaError_t status, const char *action)
{
    if (status != cudaSuccess)
        throw std::runtime_error(
            std::string(action) + ": " + cudaGetErrorString(status));
}

__global__ void rgba_to_float_nchw_kernel(
    float *output,
    const unsigned char *rgba,
    std::size_t rgba_stride,
    int width,
    int height,
    bool bgr,
    float scale,
    float mean0,
    float mean1,
    float mean2,
    float std0,
    float std1,
    float std2)
{
    const std::size_t pixel_count = static_cast<std::size_t>(width)
        * static_cast<std::size_t>(height);
    const std::size_t pixel = static_cast<std::size_t>(blockIdx.x)
        * blockDim.x + threadIdx.x;
    if (pixel >= pixel_count)
        return;

    const int x = static_cast<int>(pixel % static_cast<std::size_t>(width));
    const int y = static_cast<int>(pixel / static_cast<std::size_t>(width));
    const auto *source = rgba + static_cast<std::size_t>(y) * rgba_stride
        + static_cast<std::size_t>(x) * 4U;
    const float means[3] = {mean0, mean1, mean2};
    const float stds[3] = {std0, std1, std2};
    for (int channel = 0; channel < 3; ++channel) {
        const int source_channel = bgr ? 2 - channel : channel;
        output[static_cast<std::size_t>(channel) * pixel_count + pixel] =
            (static_cast<float>(source[source_channel]) * scale
                - means[channel])
            / stds[channel];
    }
}

} // namespace

bool ssv_tensorrt_cuda_preprocess_available() noexcept
{
    return true;
}

void ssv_tensorrt_launch_rgba_to_float_nchw(
    void *output,
    const void *rgba,
    std::size_t rgba_stride,
    const SsvPreprocessPlan &plan,
    void *stream)
{
    const auto pixel_count = static_cast<std::size_t>(plan.canvas_width)
        * static_cast<std::size_t>(plan.canvas_height);
    constexpr unsigned int threads = 256;
    const auto blocks = static_cast<unsigned int>(
        (pixel_count + threads - 1U) / threads);
    rgba_to_float_nchw_kernel<<<blocks, threads, 0,
        reinterpret_cast<cudaStream_t>(stream)>>>(
        static_cast<float *>(output),
        static_cast<const unsigned char *>(rgba),
        rgba_stride,
        plan.canvas_width,
        plan.canvas_height,
        plan.color_order == SsvInputColorOrder::Bgr,
        plan.scale,
        plan.mean[0],
        plan.mean[1],
        plan.mean[2],
        plan.std[0],
        plan.std[1],
        plan.std[2]);
    check_cuda(cudaGetLastError(), "RGBA preprocessing kernel launch failed");
}

} // namespace ssv::infer
