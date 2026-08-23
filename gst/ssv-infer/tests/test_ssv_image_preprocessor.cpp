#include "model/ssv_image_preprocessor.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace {

ssv::infer::TensorSpec input_spec(int width, int height)
{
    return {
        .name = "images",
        .dtype = ssv::infer::DataType::Float32,
        .shape = {1, 3, height, width},
        .layout = ssv::infer::TensorLayout::Nchw,
    };
}

void assert_close(float actual, float expected)
{
    assert(std::fabs(actual - expected) < 1.0e-6F);
}

void test_rgb_normalization_matches_wrapper_semantics()
{
    const auto plan = ssv::infer::ssv_make_preprocess_plan(
        input_spec(2, 2),
        {
            .color_order = ssv::SsvInputColorOrder::Rgb,
            .resize_mode = ssv::SsvResizeMode::Letterbox,
            .normalization = {
                .scale = 1.0F / 255.0F,
                .mean = {0.0F, 0.0F, 0.0F},
                .std = {1.0F, 1.0F, 1.0F},
            },
        });
    ssv::infer::SsvImagePreprocessor preprocessor(plan);
    const std::array<std::uint8_t, 16> rgba = {
        10, 20, 30, 255,
        40, 50, 60, 1,
        70, 80, 90, 2,
        100, 110, 120, 3,
    };
    std::vector<float> destination(12, -1.0F);
    const auto timing = preprocessor.run(
        {rgba, 2, 2, 8}, destination);
    static_cast<void>(timing);

    assert_close(destination[0], 10.0F / 255.0F);
    assert_close(destination[1], 40.0F / 255.0F);
    assert_close(destination[2], 70.0F / 255.0F);
    assert_close(destination[3], 100.0F / 255.0F);
    assert_close(destination[4], 20.0F / 255.0F);
    assert_close(destination[5], 50.0F / 255.0F);
    assert_close(destination[6], 80.0F / 255.0F);
    assert_close(destination[7], 110.0F / 255.0F);
    assert_close(destination[8], 30.0F / 255.0F);
    assert_close(destination[9], 60.0F / 255.0F);
    assert_close(destination[10], 90.0F / 255.0F);
    assert_close(destination[11], 120.0F / 255.0F);
}

void test_bgr_mean_std_and_buffer_reuse()
{
    ssv::infer::SsvImagePreprocessor preprocessor(
        ssv::infer::ssv_make_preprocess_plan(
            input_spec(1, 1),
            {
                .color_order = ssv::SsvInputColorOrder::Bgr,
                .resize_mode = ssv::SsvResizeMode::Stretch,
                .normalization = {
                    .scale = 1.0F,
                    .mean = {1.0F, 2.0F, 3.0F},
                    .std = {2.0F, 4.0F, 5.0F},
                },
            }));
    const std::array<std::uint8_t, 4> rgba = {10, 20, 30, 99};
    std::array<float, 3> destination = {-1.0F, -1.0F, -1.0F};
    static_cast<void>(preprocessor.run({rgba, 1, 1, 4}, destination));
    assert_close(destination[0], (30.0F - 1.0F) / 2.0F);
    assert_close(destination[1], (20.0F - 2.0F) / 4.0F);
    assert_close(destination[2], (10.0F - 3.0F) / 5.0F);
    const auto address = destination.data();
    static_cast<void>(preprocessor.run({rgba, 1, 1, 4}, destination));
    assert(destination.data() == address);
}

void test_rejects_incompatible_canvas_and_destination()
{
    ssv::infer::SsvImagePreprocessor preprocessor(
        ssv::infer::ssv_make_preprocess_plan(
            input_spec(2, 1), ssv::SsvPreprocessConfig {}));
    const std::array<std::uint8_t, 8> rgba {};
    std::array<float, 6> destination {};
    try {
        static_cast<void>(preprocessor.run({rgba, 1, 1, 4}, destination));
        assert(false);
    } catch (const std::invalid_argument &) {
    }
    try {
        static_cast<void>(preprocessor.run(
            {rgba, 2, 1, 4}, std::span<float>(destination).first(3)));
        assert(false);
    } catch (const std::invalid_argument &) {
    }
}

} // namespace

int main()
{
    test_rgb_normalization_matches_wrapper_semantics();
    test_bgr_mean_std_and_buffer_reuse();
    test_rejects_incompatible_canvas_and_destination();
    return 0;
}
