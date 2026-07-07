#include "ssv_onnxruntime_backend.hpp"

#include <numeric>
#include <stdexcept>
#include <utility>

namespace ssv::infer {

namespace {

int64_t tensor_size(const std::vector<int64_t> &shape)
{
    if (shape.empty())
        throw std::runtime_error("tensor shape is empty");
    int64_t size = 1;
    for (int64_t dim : shape) {
        if (dim <= 0)
            throw std::runtime_error("dynamic tensor shapes are not supported in this stage");
        size *= dim;
    }
    return size;
}

TensorSpec tensor_spec_from_ort(const std::string &name, const Ort::TypeInfo &type_info)
{
    auto tensor_info = type_info.GetTensorTypeAndShapeInfo();
    if (tensor_info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
        throw std::runtime_error("only float32 tensors are supported");

    TensorSpec spec;
    spec.name = name;
    spec.dtype = DataType::Float32;
    spec.shape = tensor_info.GetShape();
    if (spec.shape.size() == 4 && spec.shape[1] == 3)
        spec.layout = TensorLayout::Nchw;
    return spec;
}

std::vector<const char *> c_names(const std::vector<std::string> &names)
{
    std::vector<const char *> result;
    result.reserve(names.size());
    for (const auto &name : names)
        result.push_back(name.c_str());
    return result;
}

} // namespace

OnnxRuntimeBackend::OnnxRuntimeBackend()
    : env_(ORT_LOGGING_LEVEL_WARNING, "ssv-infer"),
      memory_info_(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault))
{
}

BackendInfo OnnxRuntimeBackend::info() const
{
    return info_;
}

ModelMetadata OnnxRuntimeBackend::load(const InferenceConfig &config)
{
    Ort::SessionOptions opts;
    opts.SetIntraOpNumThreads(1);
    opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

    info_ = {};
    info_.runtime = RuntimeKind::OnnxRuntime;
    info_.active_device_id = config.device_id;
    info_.precision = config.precision == PrecisionKind::Auto ? PrecisionKind::Fp32 : config.precision;

    bool gpu_enabled = false;
    if (config.device == DeviceKind::Auto || config.device == DeviceKind::Gpu) {
        try {
            OrtCUDAProviderOptions cuda_options{};
            cuda_options.device_id = config.device_id;
            opts.AppendExecutionProvider_CUDA(cuda_options);
            gpu_enabled = true;
        } catch (const Ort::Exception &e) {
            if (config.device == DeviceKind::Gpu) {
                throw std::runtime_error(
                    std::string("CUDAExecutionProvider unavailable: ") + e.what());
            }
        }
    }

    info_.active_device = gpu_enabled ? DeviceKind::Gpu : DeviceKind::Cpu;
    info_.provider_name = gpu_enabled ? "CUDAExecutionProvider" : "CPUExecutionProvider";

    session_ = std::make_unique<Ort::Session>(env_, config.model_path.c_str(), opts);

    Ort::AllocatorWithDefaultOptions alloc;
    input_names_.clear();
    output_names_.clear();
    metadata_ = {};
    metadata_.backend = info_;

    size_t input_count = session_->GetInputCount();
    for (size_t i = 0; i < input_count; ++i) {
        auto name = session_->GetInputNameAllocated(i, alloc);
        input_names_.push_back(name.get());
        metadata_.inputs.push_back(tensor_spec_from_ort(input_names_.back(), session_->GetInputTypeInfo(i)));
    }

    size_t output_count = session_->GetOutputCount();
    for (size_t i = 0; i < output_count; ++i) {
        auto name = session_->GetOutputNameAllocated(i, alloc);
        output_names_.push_back(name.get());
        metadata_.outputs.push_back(tensor_spec_from_ort(output_names_.back(), session_->GetOutputTypeInfo(i)));
    }

    if (metadata_.inputs.empty() || metadata_.outputs.empty())
        throw std::runtime_error("model must have at least one input and one output");

    return metadata_;
}

std::vector<Tensor> OnnxRuntimeBackend::infer(std::span<const Tensor> inputs)
{
    if (!session_)
        throw std::runtime_error("ONNX Runtime backend is not loaded");
    if (inputs.size() != input_names_.size())
        throw std::runtime_error("input tensor count does not match model inputs");

    std::vector<Ort::Value> input_values;
    input_values.reserve(inputs.size());
    for (const Tensor &input : inputs) {
        int64_t expected_size = tensor_size(input.spec.shape);
        if (expected_size != static_cast<int64_t>(input.host_data.size()))
            throw std::runtime_error("input tensor data size does not match shape");
        input_values.push_back(Ort::Value::CreateTensor<float>(
            memory_info_,
            const_cast<float *>(input.host_data.data()),
            input.host_data.size(),
            const_cast<int64_t *>(input.spec.shape.data()),
            input.spec.shape.size()));
    }

    std::vector<const char *> input_name_ptrs = c_names(input_names_);
    std::vector<const char *> output_name_ptrs = c_names(output_names_);
    std::vector<Ort::Value> output_values = session_->Run(
        Ort::RunOptions{nullptr},
        input_name_ptrs.data(), input_values.data(), input_values.size(),
        output_name_ptrs.data(), output_name_ptrs.size());

    std::vector<Tensor> outputs;
    outputs.reserve(output_values.size());
    for (size_t i = 0; i < output_values.size(); ++i) {
        auto info = output_values[i].GetTensorTypeAndShapeInfo();
        Tensor output;
        output.spec.name = i < output_names_.size() ? output_names_[i] : "";
        output.spec.dtype = DataType::Float32;
        output.spec.shape = info.GetShape();
        output.spec.layout = output.spec.shape.size() == 4 ? TensorLayout::Nchw : TensorLayout::Unknown;
        int64_t size = tensor_size(output.spec.shape);
        const float *data = output_values[i].GetTensorData<float>();
        output.host_data.assign(data, data + size);
        outputs.push_back(std::move(output));
    }
    return outputs;
}

std::unique_ptr<InferenceBackend> create_onnxruntime_backend()
{
    return std::make_unique<OnnxRuntimeBackend>();
}

} // namespace ssv::infer
