#pragma once

#include "ssv_config.hpp"
#include "core/ssv_inference_stats.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace ssv::infer {

enum class RuntimeKind { OnnxRuntime, TensorRtEngine };
enum class ModelFamily { Yolo };
enum class OutputFormat { YoloV5, YoloV8, YoloNx6 };
enum class DataType { Uint8, Float32 };
enum class TensorLayout { Nchw, Nhwc, Unknown };

struct TensorSpec {
    std::string name;
    DataType dtype = DataType::Float32;
    std::vector<int64_t> shape;
    TensorLayout layout = TensorLayout::Unknown;
};

struct SsvFloatTensorView {
    const TensorSpec *spec = nullptr;
    std::span<const float> host_data;
};

struct OnnxRuntimeBackendInfo {
    std::vector<ssv::SsvProvider> active_provider_chain;
    std::vector<SsvProviderFallbackInfo> fallbacks;
    int intra_op_threads = 1;
    int inter_op_threads = 1;
    bool allow_spinning = false;
    std::string model_hash;
    std::string session_key;
    bool session_pool_hit = false;
    SsvCacheStatus cache_status = SsvCacheStatus::Disabled;
    std::string cache_namespace;
    std::string cache_reason;
    std::string device_identity;
    std::vector<SsvNodePlacement> node_placements;
    SsvInferenceStartupTimings startup_timings;
};

struct TensorRtEngineBackendInfo {
    std::string engine_hash;
    std::string source_model_hash;
    std::string tensorrt_version;
    int cuda_runtime_version = 0;
    int compute_capability_major = -1;
    int compute_capability_minor = -1;
};

using BackendRuntimeInfo = std::variant<
    OnnxRuntimeBackendInfo,
    TensorRtEngineBackendInfo>;

struct BackendInfo {
    BackendRuntimeInfo runtime = OnnxRuntimeBackendInfo {};
    int active_device_id = 0;
    std::optional<ssv::SsvPrecision> resolved_precision;
};

struct ModelMetadata {
    BackendInfo backend;
    std::vector<TensorSpec> inputs;
    std::vector<TensorSpec> outputs;
    std::unordered_map<std::string, std::string> properties;
};

} // namespace ssv::infer
