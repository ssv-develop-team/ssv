#include "model/ssv_image_preprocessor.hpp"

#include "ssv_meta.hpp"

#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ssv::infer {
namespace {

using Clock = std::chrono::steady_clock;

std::size_t checked_element_count(int width, int height)
{
    if (width <= 0 || height <= 0)
        throw std::invalid_argument("preprocess canvas dimensions must be positive");
    const auto area = static_cast<std::size_t>(width)
        * static_cast<std::size_t>(height);
    if (area > std::numeric_limits<std::size_t>::max() / 3U)
        throw std::invalid_argument("preprocess tensor element count overflows");
    return area * 3U;
}

void validate_input(const TensorSpec &input)
{
    if (input.dtype != DataType::Float32)
        throw std::invalid_argument("preprocess input must use float32");
    if (input.layout != TensorLayout::Nchw)
        throw std::invalid_argument("preprocess input must use NCHW layout");
    if (input.shape.size() != 4 || input.shape[0] != 1
        || input.shape[1] != 3 || input.shape[2] <= 0
        || input.shape[3] <= 0) {
        throw std::invalid_argument(
            "preprocess input must be static float32 [1,3,H,W]");
    }
    if (input.shape[2] > std::numeric_limits<int>::max()
        || input.shape[3] > std::numeric_limits<int>::max()) {
        throw std::invalid_argument("preprocess input dimensions are too large");
    }
}

void validate_normalization(const SsvPreprocessConfig &config)
{
    if (!std::isfinite(config.normalization.scale)
        || config.normalization.scale <= 0.0F) {
        throw std::invalid_argument("preprocess normalization scale must be finite and positive");
    }
    for (const float value : config.normalization.mean) {
        if (!std::isfinite(value))
            throw std::invalid_argument("preprocess normalization mean must be finite");
    }
    for (const float value : config.normalization.std) {
        if (!std::isfinite(value) || value <= 0.0F)
            throw std::invalid_argument("preprocess normalization std must be finite and positive");
    }
}

} // namespace

SsvPreprocessPlan ssv_make_preprocess_plan(
    const TensorSpec &input,
    const SsvPreprocessConfig &config)
{
    validate_input(input);
    validate_normalization(config);
    SsvPreprocessPlan plan;
    plan.input = input;
    plan.canvas_height = static_cast<int>(input.shape[2]);
    plan.canvas_width = static_cast<int>(input.shape[3]);
    plan.color_order = config.color_order;
    plan.resize_mode = config.resize_mode;
    plan.execution = config.execution;
    plan.scale = config.normalization.scale;
    plan.mean = config.normalization.mean;
    plan.std = config.normalization.std;
    static_cast<void>(checked_element_count(
        plan.canvas_width, plan.canvas_height));
    return plan;
}

SsvImagePreprocessor::SsvImagePreprocessor(SsvPreprocessPlan plan)
    : plan_(std::move(plan))
{
    validate_input(plan_.input);
    static_cast<void>(checked_element_count(
        plan_.canvas_width, plan_.canvas_height));
}

SsvPreprocessTiming SsvImagePreprocessor::run(
    const SsvRgbaFrameView &canvas,
    std::span<float> destination) const
{
    if (canvas.width != plan_.canvas_width
        || canvas.height != plan_.canvas_height) {
        throw std::invalid_argument(
            "RGBA canvas dimensions do not match model input contract");
    }
    const auto row_bytes = static_cast<std::size_t>(canvas.width) * 4U;
    if (canvas.stride < row_bytes)
        throw std::invalid_argument("RGBA canvas stride is smaller than one row");
    const auto last_row = static_cast<std::size_t>(canvas.height - 1)
        * canvas.stride;
    if (last_row > std::numeric_limits<std::size_t>::max() - row_bytes
        || last_row + row_bytes > canvas.bytes.size()) {
        throw std::invalid_argument("RGBA canvas bytes do not cover its layout");
    }
    const auto element_count = checked_element_count(
        plan_.canvas_width, plan_.canvas_height);
    if (destination.size() != element_count)
        throw std::invalid_argument(
            "preprocess destination size does not match model input");

    const auto started = Clock::now();
    const bool bgr = plan_.color_order == SsvInputColorOrder::Bgr;
    for (int channel = 0; channel < 3; ++channel) {
        const int source_channel = bgr ? 2 - channel : channel;
        const float mean = plan_.mean[static_cast<std::size_t>(channel)];
        const float std = plan_.std[static_cast<std::size_t>(channel)];
        for (int y = 0; y < plan_.canvas_height; ++y) {
            const auto *row = canvas.bytes.data()
                + static_cast<std::size_t>(y) * canvas.stride;
            auto *output = destination.data()
                + (static_cast<std::size_t>(channel)
                    * static_cast<std::size_t>(plan_.canvas_height)
                    + static_cast<std::size_t>(y))
                    * static_cast<std::size_t>(plan_.canvas_width);
            for (int x = 0; x < plan_.canvas_width; ++x) {
                const auto pixel = row[static_cast<std::size_t>(x) * 4U
                    + static_cast<std::size_t>(source_channel)];
                output[x] = (static_cast<float>(pixel) * plan_.scale - mean)
                    / std;
            }
        }
    }
    return {
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                Clock::now() - started)
                .count()),
    };
}

} // namespace ssv::infer
