#include "model/ssv_yolo_parser.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <span>
#include <string>
#include <vector>

namespace {

bool nearly_equal(float left, float right)
{
    return std::fabs(left - right) < 0.0001F;
}

void assert_box(
    const SsvDetection &detection,
    float x1,
    float y1,
    float x2,
    float y2)
{
    assert(nearly_equal(detection.x1, x1));
    assert(nearly_equal(detection.y1, y1));
    assert(nearly_equal(detection.x2, x2));
    assert(nearly_equal(detection.y2, y2));
}

std::vector<SsvDetection> parse_nx6(
    std::span<const float> values,
    const PreprocessTransform &transform)
{
    ssv::infer::InferenceConfig config;
    config.output_format = ssv::infer::OutputFormat::YoloNx6;
    config.confidence_threshold = 0.5F;

    ssv::infer::ModelMetadata metadata;
    metadata.outputs.push_back({
        "output0",
        ssv::infer::DataType::Float32,
        {1, static_cast<std::int64_t>(values.size() / 6), 6},
        ssv::infer::TensorLayout::Unknown,
    });

    ssv::infer::YoloOutputParser parser;
    parser.configure(config, metadata, {"person"});
    const ssv::infer::SsvFloatTensorView output {
        &metadata.outputs.front(),
        values,
    };
    return parser.parse(
        std::span<const ssv::infer::SsvFloatTensorView>(&output, 1),
        transform);
}

void test_uses_each_frame_exact_transform()
{
    const std::array<float, 6> landscape_output {
        64.0F, 176.0F, 320.0F, 320.0F, 0.9F, 0.0F,
    };
    const PreprocessTransform landscape {
        640, 360, 640, 640, 1.0F, 1.0F, 0, 140, 0, 140,
    };
    const auto landscape_detections = parse_nx6(
        landscape_output, landscape);
    assert(landscape_detections.size() == 1);
    assert_box(landscape_detections.front(), 0.1F, 0.1F, 0.5F, 0.5F);

    const std::array<float, 6> portrait_output {
        176.0F, 64.0F, 320.0F, 320.0F, 0.9F, 0.0F,
    };
    const PreprocessTransform portrait {
        360, 640, 640, 640, 1.0F, 1.0F, 140, 0, 140, 0,
    };
    const auto portrait_detections = parse_nx6(
        portrait_output, portrait);
    assert(portrait_detections.size() == 1);
    assert_box(portrait_detections.front(), 0.1F, 0.1F, 0.5F, 0.5F);

    const std::array<float, 6> odd_padding_output {
        25.0F, 15.0F, 185.0F, 95.0F, 0.9F, 0.0F,
    };
    const PreprocessTransform odd_padding {
        100, 50, 211, 111, 2.0F, 2.0F, 5, 5, 6, 6,
    };
    const auto odd_padding_detections = parse_nx6(
        odd_padding_output, odd_padding);
    assert(odd_padding_detections.size() == 1);
    assert_box(
        odd_padding_detections.front(), 0.1F, 0.1F, 0.9F, 0.9F);
}

void test_stretch_uses_independent_axis_scales()
{
    const auto transform = ssv_make_resize_transform(
        1280,
        720,
        640,
        640,
        ssv::SsvResizeMode::Stretch);
    assert(nearly_equal(transform.scale_x, 0.5F));
    assert(nearly_equal(transform.scale_y, 640.0F / 720.0F));
    assert(transform.pad_left == 0);
    assert(transform.pad_top == 0);

    const std::array<float, 6> output {
        64.0F, 128.0F, 576.0F, 512.0F, 0.9F, 0.0F,
    };
    const auto detections = parse_nx6(output, transform);
    assert(detections.size() == 1);
    assert_box(detections.front(), 0.1F, 0.2F, 0.9F, 0.8F);
}

void test_clips_source_boxes_and_drops_boxes_outside_the_source()
{
    const std::array<float, 12> output {
        -20.0F, -30.0F, 230.0F, 150.0F, 0.9F, 0.0F,
        -10.0F, 20.0F, 4.0F, 40.0F, 0.8F, 0.0F,
    };
    const PreprocessTransform transform {
        100, 50, 211, 111, 2.0F, 2.0F, 5, 5, 6, 6,
    };

    const auto detections = parse_nx6(output, transform);

    assert(detections.size() == 1);
    assert_box(detections.front(), 0.0F, 0.0F, 1.0F, 1.0F);
}

void test_runs_nms_before_source_clipping()
{
    const std::array<float, 12> output {
        100.0F, 0.0F, 300.0F, 180.0F, 0.9F, 0.0F,
        100.0F, 100.0F, 300.0F, 220.0F, 0.8F, 0.0F,
    };
    const PreprocessTransform transform {
        640, 360, 640, 640, 1.0F, 1.0F, 0, 140, 0, 140,
    };

    const auto detections = parse_nx6(output, transform);

    assert(detections.size() == 2);
}

void test_yolo_parser_parses_nx6_output()
{
    ssv::infer::InferenceConfig config;
    config.output_format = ssv::infer::OutputFormat::YoloNx6;
    config.confidence_threshold = 0.5F;
    config.target_class = "head";

    ssv::infer::ModelMetadata metadata;
    ssv::infer::TensorSpec output_spec;
    output_spec.name = "output0";
    output_spec.shape = {1, 2, 6};
    metadata.outputs.push_back(output_spec);

    ssv::infer::YoloOutputParser parser;
    parser.configure(config, metadata, {"helmet", "head"});

    const std::array<float, 12> output_data = {
        0.1F, 0.2F, 0.4F, 0.8F, 0.9F, 1.0F,
        0.2F, 0.2F, 0.5F, 0.8F, 0.4F, 1.0F,
    };
    const ssv::infer::SsvFloatTensorView output {
        &output_spec,
        std::span<const float>(output_data),
    };

    const PreprocessTransform transform {
        640, 640, 640, 640, 1.0F, 1.0F, 0, 0, 0, 0};

    auto detections = parser.parse(
        std::span<const ssv::infer::SsvFloatTensorView>(&output, 1),
        transform);

    assert(detections.size() == 1);
    assert(detections[0].class_id == 1);
    assert(std::string(detections[0].class_name) == "head");
    assert(nearly_equal(detections[0].confidence, 0.9F));
}

} // namespace

int main()
{
    test_uses_each_frame_exact_transform();
    test_stretch_uses_independent_axis_scales();
    test_clips_source_boxes_and_drops_boxes_outside_the_source();
    test_runs_nms_before_source_clipping();
    test_yolo_parser_parses_nx6_output();
    return 0;
}
