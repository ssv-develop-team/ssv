#include "ssv_inference_test_service.hpp"

#include "core/ssv_inference_service_internal.hpp"
#include "core/ssv_inference_backend.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace ssv::infer {
namespace {

ModelMetadata make_test_wrapper_metadata()
{
    ModelMetadata metadata;
    metadata.inputs.push_back({
        "images_rgba",
        DataType::Uint8,
        {1, 2, 3, 4},
        TensorLayout::Nhwc,
    });
    metadata.outputs.push_back({
        "output0",
        DataType::Float32,
        {1, 1, 6},
        TensorLayout::Unknown,
    });
    metadata.properties = {
        {"ssv.wrapper.channel_rule", "drop_alpha_keep_rgb"},
        {"ssv.wrapper.contract", "rgba_u8_nhwc_v1"},
        {"ssv.wrapper.dtype", "uint8"},
        {"ssv.wrapper.height", "2"},
        {"ssv.wrapper.layout", "NHWC"},
        {"ssv.wrapper.model_family", "yolo"},
        {"ssv.wrapper.normalization", "divide_by_255"},
        {"ssv.wrapper.output_format", "yolo_nx6"},
        {"ssv.wrapper.source_sha256", std::string(64, 'b')},
        {"ssv.wrapper.tool", "ssv.prepare_wrapper"},
        {"ssv.wrapper.tool_version", "1.0.0"},
        {"ssv.wrapper.width", "3"},
    };
    return metadata;
}

class TestServiceBackend final : public InferenceBackend {
public:
    explicit TestServiceBackend(SsvInferenceTestServiceOptions options)
        : detection_sequence_(std::move(options.detection_sequence))
        , on_destroyed_(std::move(options.on_backend_destroyed))
    {
    }

    ~TestServiceBackend() override
    {
        if (!on_destroyed_)
            return;
        try {
            on_destroyed_();
        } catch (...) {
            // Test observation must not change service cleanup semantics.
        }
    }

    BackendInfo info() const override { return {}; }

    ModelMetadata load(
        const InferenceConfig &,
        SsvInferenceBufferAllocator &allocator) override
    {
        metadata_ = make_test_wrapper_metadata();
        output_buffer_ = allocator.allocate(
            6 * sizeof(float), alignof(float));
        output_view_ = {
            &metadata_.outputs.front(),
            output_buffer_.as_span<float>(),
        };
        return metadata_;
    }

    std::span<const SsvFloatTensorView> infer(
        const SsvUint8TensorView &,
        std::stop_token) override
    {
        static constexpr SsvInferenceTestDetection default_detection;
        const auto &detection = detection_sequence_.empty()
            ? default_detection
            : detection_sequence_[std::min(
                  next_detection_, detection_sequence_.size() - 1)];
        ++next_detection_;
        auto output = output_buffer_.as_span<float>();
        output[0] = detection.x1;
        output[1] = detection.y1;
        output[2] = detection.x2;
        output[3] = detection.y2;
        output[4] = detection.confidence;
        output[5] = static_cast<float>(detection.class_id);
        return {&output_view_, 1};
    }

private:
    ModelMetadata metadata_;
    SsvInferenceBuffer output_buffer_;
    SsvFloatTensorView output_view_;
    std::vector<SsvInferenceTestDetection> detection_sequence_;
    std::function<void()> on_destroyed_;
    std::size_t next_detection_ = 0;
};

} // namespace

SsvInferenceServicePtr ssv_inference_test_service_create(
    const ssv::SsvInferenceConfig &config,
    SsvInferenceTestServiceOptions options)
{
    return ssv_inference_service_create_with_backend(
        config, std::make_unique<TestServiceBackend>(std::move(options)));
}

} // namespace ssv::infer
