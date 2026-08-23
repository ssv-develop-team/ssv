#include "backends/tensorrt/ssv_tensorrt_cuda_kernel.hpp"
#include "model/ssv_image_preprocessor.hpp"

#include <cuda_runtime_api.h>

#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

void check_cuda(cudaError_t status)
{
    assert(status == cudaSuccess);
}

void test_rgba_to_float_nchw_matches_cpu_reference()
{
    int device_count = 0;
    if (cudaGetDeviceCount(&device_count) != cudaSuccess || device_count == 0)
        return;
    check_cuda(cudaSetDevice(0));

    ssv::infer::SsvPreprocessPlan plan;
    plan.input.name = "images";
    plan.input.dtype = ssv::infer::DataType::Float32;
    plan.input.layout = ssv::infer::TensorLayout::Nchw;
    plan.input.shape = {1, 3, 2, 2};
    plan.canvas_width = 2;
    plan.canvas_height = 2;
    plan.color_order = ssv::SsvInputColorOrder::Bgr;
    plan.scale = 0.5F;
    plan.mean = {1.0F, 2.0F, 3.0F};
    plan.std = {2.0F, 4.0F, 5.0F};

    constexpr std::size_t width = 2;
    constexpr std::size_t height = 2;
    constexpr std::size_t stride = 12;
    const std::array<std::uint8_t, stride * height> host_rgba {
        10, 20, 30, 255, 40, 50, 60, 255, 0, 0, 0, 0,
        70, 80, 90, 255, 100, 110, 120, 255, 0, 0, 0, 0,
    };
    std::array<float, 3 * width * height> expected {};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const auto source_channel = 2U - channel;
        for (std::size_t y = 0; y < height; ++y) {
            for (std::size_t x = 0; x < width; ++x) {
                const auto pixel = y * width + x;
                const auto source = host_rgba[y * stride + x * 4U
                    + source_channel];
                expected[channel * width * height + pixel] =
                    (static_cast<float>(source) * plan.scale
                        - plan.mean[channel])
                    / plan.std[channel];
            }
        }
    }

    void *device_rgba = nullptr;
    void *device_output = nullptr;
    check_cuda(cudaMalloc(&device_rgba, host_rgba.size()));
    check_cuda(cudaMalloc(&device_output, expected.size() * sizeof(float)));
    check_cuda(cudaMemcpy(
        device_rgba,
        host_rgba.data(),
        host_rgba.size(),
        cudaMemcpyHostToDevice));

    ssv::infer::ssv_tensorrt_launch_rgba_to_float_nchw(
        device_output, device_rgba, stride, plan, nullptr);
    check_cuda(cudaDeviceSynchronize());

    std::array<float, expected.size()> actual {};
    check_cuda(cudaMemcpy(
        actual.data(),
        device_output,
        actual.size() * sizeof(float),
        cudaMemcpyDeviceToHost));
    for (std::size_t index = 0; index < actual.size(); ++index)
        assert(std::fabs(actual[index] - expected[index]) < 1.0e-6F);

    check_cuda(cudaFree(device_output));
    check_cuda(cudaFree(device_rgba));
}

} // namespace

int main()
{
    test_rgba_to_float_nchw_matches_cpu_reference();
    return 0;
}
