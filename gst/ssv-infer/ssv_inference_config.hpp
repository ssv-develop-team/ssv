#pragma once

#include "ssv_tensor.hpp"

#include <string>

namespace ssv::infer {

struct InferenceConfig {
    RuntimeKind runtime = RuntimeKind::Auto;
    std::string model_path;
    DeviceKind device = DeviceKind::Auto;
    int device_id = 0;
    PrecisionKind precision = PrecisionKind::Auto;
    ModelFamily model_family = ModelFamily::Yolo;
    OutputFormat output_format = OutputFormat::Auto;
    float confidence_threshold = 0.5f;
    std::string target_class = "person";
    std::string label_map = "config/model-labels/coco80.txt";
};

RuntimeKind parse_runtime_kind(const std::string &value);
DeviceKind parse_device_kind(const std::string &value);
PrecisionKind parse_precision_kind(const std::string &value);
ModelFamily parse_model_family(const std::string &value);
OutputFormat parse_output_format(const std::string &value);

std::string to_string(RuntimeKind value);
std::string to_string(DeviceKind value);
std::string to_string(PrecisionKind value);
std::string to_string(ModelFamily value);
std::string to_string(OutputFormat value);

RuntimeKind resolve_runtime(RuntimeKind requested, const std::string &model_path);
void validate_inference_config(const InferenceConfig &config);

} // namespace ssv::infer
