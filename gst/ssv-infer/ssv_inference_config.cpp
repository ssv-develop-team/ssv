#include "ssv_inference_config.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <stdexcept>

namespace ssv::infer {

namespace {

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool ends_with(const std::string &value, const std::string &suffix)
{
    return value.size() >= suffix.size() &&
        value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

} // namespace

RuntimeKind parse_runtime_kind(const std::string &value)
{
    std::string v = lower(value);
    if (v == "auto")
        return RuntimeKind::Auto;
    if (v == "onnxruntime")
        return RuntimeKind::OnnxRuntime;
    if (v == "tensorrt")
        return RuntimeKind::TensorRt;
    throw std::invalid_argument("unsupported inference runtime: " + value);
}

DeviceKind parse_device_kind(const std::string &value)
{
    std::string v = lower(value);
    if (v == "auto")
        return DeviceKind::Auto;
    if (v == "cpu")
        return DeviceKind::Cpu;
    if (v == "gpu")
        return DeviceKind::Gpu;
    throw std::invalid_argument("unsupported inference device: " + value);
}

PrecisionKind parse_precision_kind(const std::string &value)
{
    std::string v = lower(value);
    if (v == "auto")
        return PrecisionKind::Auto;
    if (v == "fp32")
        return PrecisionKind::Fp32;
    if (v == "fp16")
        return PrecisionKind::Fp16;
    if (v == "int8")
        return PrecisionKind::Int8;
    throw std::invalid_argument("unsupported inference precision: " + value);
}

ModelFamily parse_model_family(const std::string &value)
{
    std::string v = lower(value);
    if (v == "auto")
        return ModelFamily::Auto;
    if (v == "yolo")
        return ModelFamily::Yolo;
    throw std::invalid_argument("unsupported model family: " + value);
}

OutputFormat parse_output_format(const std::string &value)
{
    std::string v = lower(value);
    if (v == "auto")
        return OutputFormat::Auto;
    if (v == "yolov5")
        return OutputFormat::YoloV5;
    if (v == "yolov8")
        return OutputFormat::YoloV8;
    if (v == "yolo_nx6")
        return OutputFormat::YoloNx6;
    throw std::invalid_argument("unsupported output format: " + value);
}

std::string to_string(RuntimeKind value)
{
    switch (value) {
    case RuntimeKind::Auto: return "auto";
    case RuntimeKind::OnnxRuntime: return "onnxruntime";
    case RuntimeKind::TensorRt: return "tensorrt";
    }
    return "auto";
}

std::string to_string(DeviceKind value)
{
    switch (value) {
    case DeviceKind::Auto: return "auto";
    case DeviceKind::Cpu: return "cpu";
    case DeviceKind::Gpu: return "gpu";
    }
    return "auto";
}

std::string to_string(PrecisionKind value)
{
    switch (value) {
    case PrecisionKind::Auto: return "auto";
    case PrecisionKind::Fp32: return "fp32";
    case PrecisionKind::Fp16: return "fp16";
    case PrecisionKind::Int8: return "int8";
    }
    return "auto";
}

std::string to_string(ModelFamily value)
{
    switch (value) {
    case ModelFamily::Auto: return "auto";
    case ModelFamily::Yolo: return "yolo";
    }
    return "auto";
}

std::string to_string(OutputFormat value)
{
    switch (value) {
    case OutputFormat::Auto: return "auto";
    case OutputFormat::YoloV5: return "yolov5";
    case OutputFormat::YoloV8: return "yolov8";
    case OutputFormat::YoloNx6: return "yolo_nx6";
    }
    return "auto";
}

RuntimeKind resolve_runtime(RuntimeKind requested, const std::string &model_path)
{
    if (requested != RuntimeKind::Auto)
        return requested;
    std::string path = lower(model_path);
    if (ends_with(path, ".onnx"))
        return RuntimeKind::OnnxRuntime;
    if (ends_with(path, ".engine"))
        return RuntimeKind::TensorRt;
    throw std::invalid_argument(
        "runtime=auto requires model_path ending in .onnx or .engine: " + model_path);
}

void validate_inference_config(const InferenceConfig &config)
{
    if (config.model_path.empty())
        throw std::invalid_argument("model-path not set (use mock-detect=true for testing)");
    if (!std::filesystem::exists(config.model_path))
        throw std::invalid_argument("model file not found: " + config.model_path);
    if (config.device_id < 0)
        throw std::invalid_argument("device-id must be non-negative");
    if (config.confidence_threshold < 0.0f || config.confidence_threshold > 1.0f)
        throw std::invalid_argument("conf-threshold must be in [0, 1]");
    if (config.model_family != ModelFamily::Auto && config.model_family != ModelFamily::Yolo)
        throw std::invalid_argument("only YOLO model family is supported");

    RuntimeKind runtime = resolve_runtime(config.runtime, config.model_path);
    if (runtime == RuntimeKind::TensorRt && config.device == DeviceKind::Cpu)
        throw std::invalid_argument("runtime=tensorrt does not support device=cpu");
    if (runtime == RuntimeKind::OnnxRuntime &&
        config.precision != PrecisionKind::Auto &&
        config.precision != PrecisionKind::Fp32) {
        throw std::invalid_argument("runtime=onnxruntime only supports precision=auto or fp32");
    }
}

} // namespace ssv::infer
