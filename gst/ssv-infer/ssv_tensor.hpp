#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ssv::infer {

enum class RuntimeKind { Auto, OnnxRuntime, TensorRt };
enum class DeviceKind { Auto, Cpu, Gpu };
enum class PrecisionKind { Auto, Fp32, Fp16, Int8 };
enum class ModelFamily { Auto, Yolo };
enum class OutputFormat { Auto, YoloV5, YoloV8, YoloNx6 };
enum class DataType { Float32 };
enum class TensorLayout { Nchw, Nhwc, Unknown };

struct TensorSpec {
    std::string name;
    DataType dtype = DataType::Float32;
    std::vector<int64_t> shape;
    TensorLayout layout = TensorLayout::Unknown;
};

struct Tensor {
    TensorSpec spec;
    std::vector<float> host_data;
};

struct BackendInfo {
    RuntimeKind runtime = RuntimeKind::Auto;
    DeviceKind active_device = DeviceKind::Auto;
    int active_device_id = 0;
    PrecisionKind precision = PrecisionKind::Auto;
    std::string provider_name;
};

struct ModelMetadata {
    BackendInfo backend;
    std::vector<TensorSpec> inputs;
    std::vector<TensorSpec> outputs;
};

struct SsvVideoFrame {
    uint64_t frame_id = 0;
    std::string source_id;
    int width = 0;
    int height = 0;
    int stride = 0;
    std::vector<uint8_t> bgr;
};

} // namespace ssv::infer
