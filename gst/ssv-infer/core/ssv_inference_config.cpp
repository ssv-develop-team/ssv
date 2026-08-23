#include "core/ssv_inference_config.hpp"

#include <filesystem>
#include <stdexcept>
#include <variant>

namespace ssv::infer {
namespace {

ModelFamily model_family_from_config(const std::string &value)
{
    if (value == "yolo")
        return ModelFamily::Yolo;
    throw std::invalid_argument("unsupported model family: " + value);
}

OutputFormat output_format_from_config(const std::string &value)
{
    if (value == "yolov5")
        return OutputFormat::YoloV5;
    if (value == "yolov8")
        return OutputFormat::YoloV8;
    if (value == "yolo_nx6")
        return OutputFormat::YoloNx6;
    throw std::invalid_argument("unsupported output format: " + value);
}

bool blank(const std::string &value)
{
    return value.find_first_not_of(" \t\r\n") == std::string::npos;
}

} // namespace

InferenceConfig make_inference_config(
    const ssv::SsvInferenceConfig &source)
{
    if (!source.enabled) {
        throw std::invalid_argument(
            "cannot start inference service when inference is disabled");
    }

    InferenceConfig config;
    config.model_path = source.model.path;
    config.model_manifest = source.model.manifest;
    config.model_family = model_family_from_config(source.model.family);
    config.output_format =
        output_format_from_config(source.model.output_format);
    config.label_map = source.model.label_map;
    config.confidence_threshold = source.confidence_threshold;
    config.target_class = source.target_class;
    config.preprocess = source.model.preprocess;

    if (const auto *onnx =
            std::get_if<ssv::SsvOnnxRuntimeConfig>(&source.runtime)) {
        config.runtime = RuntimeKind::OnnxRuntime;
        config.providers = onnx->providers;
        config.device_id = onnx->device_id;
        config.precision = onnx->precision;
        config.cpu_threads = onnx->cpu_threads;
        config.cache = onnx->cache;
    } else {
        const auto &tensorrt =
            std::get<ssv::SsvTensorRtEngineConfig>(source.runtime);
        config.runtime = RuntimeKind::TensorRtEngine;
        config.device_id = tensorrt.device_id;
    }

    validate_inference_config(config);
    return config;
}

void validate_inference_config(const InferenceConfig &config)
{
    if (blank(config.model_path))
        throw std::invalid_argument("inference.model.path must not be empty");
    if (!std::filesystem::exists(config.model_path)) {
        throw std::invalid_argument(
            "model file not found: " + config.model_path);
    }
    if (blank(config.label_map)) {
        throw std::invalid_argument(
            "inference.model.label_map must not be empty");
    }
    if (!config.preprocess) {
        throw std::invalid_argument(
            "inference.model.preprocess is required when inference is enabled");
    }
    if (config.device_id < 0)
        throw std::invalid_argument("runtime device_id must be non-negative");
    if (config.confidence_threshold < 0.0F
        || config.confidence_threshold > 1.0F) {
        throw std::invalid_argument(
            "inference.confidence_threshold must be in [0, 1]");
    }

    if (config.runtime == RuntimeKind::OnnxRuntime) {
        if (config.model_manifest) {
            throw std::invalid_argument(
                "onnxruntime must not define inference.model.manifest");
        }
        if (config.providers.mode == ssv::SsvProviderMode::Auto
            && !config.providers.order.empty()) {
            throw std::invalid_argument(
                "auto providers must not define an order");
        }
        if (config.providers.mode == ssv::SsvProviderMode::Explicit
            && config.providers.order.empty()) {
            throw std::invalid_argument(
                "explicit providers require a non-empty order");
        }
        return;
    }

    if (!config.model_manifest || blank(*config.model_manifest)) {
        throw std::invalid_argument(
            "tensorrt-engine requires inference.model.manifest");
    }
    if (!std::filesystem::exists(*config.model_manifest)) {
        throw std::invalid_argument(
            "manifest file not found: " + *config.model_manifest);
    }
    if (!std::filesystem::is_regular_file(*config.model_manifest)) {
        throw std::invalid_argument(
            "manifest path is not a regular file: "
            + *config.model_manifest);
    }
}

} // namespace ssv::infer
