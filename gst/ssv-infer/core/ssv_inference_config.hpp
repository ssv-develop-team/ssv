#pragma once

#include "core/ssv_tensor.hpp"

#include <optional>
#include <string>

namespace ssv::infer {

struct InferenceConfig {
    RuntimeKind runtime = RuntimeKind::OnnxRuntime;
    std::string model_path;
    std::optional<std::string> model_manifest;
    ssv::SsvProviderConfig providers;
    int device_id = 0;
    ssv::SsvPrecision precision = ssv::SsvPrecision::Auto;
    std::optional<int> cpu_threads;
    ssv::SsvCacheConfig cache;
    ModelFamily model_family = ModelFamily::Yolo;
    OutputFormat output_format = OutputFormat::YoloV8;
    float confidence_threshold = 0.5f;
    std::string target_class = "person";
    std::string label_map = "config/model-labels/coco80.txt";
    std::optional<ssv::SsvPreprocessConfig> preprocess;
};

[[nodiscard]] InferenceConfig make_inference_config(
    const ssv::SsvInferenceConfig &config);
void validate_inference_config(const InferenceConfig &config);

} // namespace ssv::infer
