#pragma once

#include "core/ssv_tensor.hpp"
#include "ssv_meta.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace ssv::infer {

struct SsvPreprocessPlan {
    TensorSpec input;
    int canvas_width = 0;
    int canvas_height = 0;
    SsvInputColorOrder color_order = SsvInputColorOrder::Rgb;
    SsvResizeMode resize_mode = SsvResizeMode::Letterbox;
    SsvPreprocessExecution execution = SsvPreprocessExecution::Auto;
    float scale = 1.0F;
    std::array<float, 3> mean {0.0F, 0.0F, 0.0F};
    std::array<float, 3> std {1.0F, 1.0F, 1.0F};
};

struct SsvPreprocessTiming {
    std::uint64_t normalize_layout_us = 0;
};

[[nodiscard]] SsvPreprocessPlan ssv_make_preprocess_plan(
    const TensorSpec &input,
    const SsvPreprocessConfig &config);

class SsvImagePreprocessor final {
public:
    explicit SsvImagePreprocessor(SsvPreprocessPlan plan);

    [[nodiscard]] SsvPreprocessTiming run(
        const SsvRgbaFrameView &canvas,
        std::span<float> destination) const;

    [[nodiscard]] const SsvPreprocessPlan &plan() const noexcept
    {
        return plan_;
    }

private:
    SsvPreprocessPlan plan_;
};

} // namespace ssv::infer
