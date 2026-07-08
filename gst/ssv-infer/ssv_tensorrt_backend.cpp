#include "ssv_inference_backend.hpp"

#include <NvInfer.h>
#include <NvInferVersion.h>
#include <cuda_runtime_api.h>

#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ssv::infer {

namespace {

class TrtLogger final : public nvinfer1::ILogger {
public:
    void log(Severity severity, const char *msg) noexcept override
    {
        if (severity <= Severity::kWARNING)
            last_message_ = msg ? msg : "";
    }

    std::string last_message() const { return last_message_; }

private:
    std::string last_message_;
};

template <typename T>
using TrtPtr = std::unique_ptr<T>;

struct TrtTensorBinding {
    std::string name;
    TensorSpec spec;
    bool input = false;
};

void check_cuda(cudaError_t status, const char *action)
{
    if (status != cudaSuccess)
        throw std::runtime_error(std::string(action) + ": " + cudaGetErrorString(status));
}

std::vector<char> read_file(const std::string &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("TensorRT engine not found: " + path);
    return std::vector<char>(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

std::vector<int64_t> shape_from_dims(const nvinfer1::Dims &dims)
{
    std::vector<int64_t> shape;
    shape.reserve(dims.nbDims);
    for (int i = 0; i < dims.nbDims; ++i)
        shape.push_back(dims.d[i]);
    return shape;
}

#if NV_TENSORRT_MAJOR >= 10
TensorSpec spec_from_tensor(nvinfer1::ICudaEngine &engine, const char *name)
{
    if (engine.getTensorDataType(name) != nvinfer1::DataType::kFLOAT)
        throw std::runtime_error("only float32 TensorRT bindings are supported");
    TensorSpec spec;
    spec.name = name;
    spec.dtype = DataType::Float32;
    spec.shape = shape_from_dims(engine.getTensorShape(name));
    if (spec.shape.size() == 4 && spec.shape[1] == 3)
        spec.layout = TensorLayout::Nchw;
    return spec;
}

std::vector<TrtTensorBinding> collect_bindings(nvinfer1::ICudaEngine &engine)
{
    std::vector<TrtTensorBinding> bindings;
    int32_t count = engine.getNbIOTensors();
    bindings.reserve(static_cast<size_t>(count));
    for (int32_t i = 0; i < count; ++i) {
        const char *name = engine.getIOTensorName(i);
        if (!name)
            throw std::runtime_error("TensorRT engine has unnamed IO tensor");
        nvinfer1::TensorIOMode mode = engine.getTensorIOMode(name);
        if (mode != nvinfer1::TensorIOMode::kINPUT && mode != nvinfer1::TensorIOMode::kOUTPUT)
            continue;
        bindings.push_back({name, spec_from_tensor(engine, name), mode == nvinfer1::TensorIOMode::kINPUT});
    }
    return bindings;
}
#else
TensorSpec spec_from_binding(nvinfer1::ICudaEngine &engine, int binding)
{
    if (engine.getBindingDataType(binding) != nvinfer1::DataType::kFLOAT)
        throw std::runtime_error("only float32 TensorRT bindings are supported");
    TensorSpec spec;
    spec.name = engine.getBindingName(binding);
    spec.dtype = DataType::Float32;
    spec.shape = shape_from_dims(engine.getBindingDimensions(binding));
    if (spec.shape.size() == 4 && spec.shape[1] == 3)
        spec.layout = TensorLayout::Nchw;
    return spec;
}

std::vector<TrtTensorBinding> collect_bindings(nvinfer1::ICudaEngine &engine)
{
    std::vector<TrtTensorBinding> bindings;
    int count = engine.getNbBindings();
    bindings.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
        bindings.push_back({engine.getBindingName(i), spec_from_binding(engine, i), engine.bindingIsInput(i)});
    return bindings;
}
#endif

} // namespace

class TensorRtBackend final : public InferenceBackend {
public:
    BackendInfo info() const override { return info_; }
    ModelMetadata load(const InferenceConfig &config) override;
    std::vector<Tensor> infer(std::span<const Tensor> inputs) override;

private:
    TrtLogger logger_;
    TrtPtr<nvinfer1::IRuntime> runtime_{nullptr};
    TrtPtr<nvinfer1::ICudaEngine> engine_{nullptr};
    TrtPtr<nvinfer1::IExecutionContext> context_{nullptr};
    BackendInfo info_;
    ModelMetadata metadata_;
    TrtTensorBinding input_;
    std::vector<TrtTensorBinding> outputs_;
};

ModelMetadata TensorRtBackend::load(const InferenceConfig &config)
{
    if (config.device == DeviceKind::Cpu)
        throw std::runtime_error("runtime=tensorrt does not support device=cpu");
    check_cuda(cudaSetDevice(config.device_id), "cudaSetDevice failed");

    std::vector<char> engine_bytes = read_file(config.model_path);
    runtime_.reset(nvinfer1::createInferRuntime(logger_));
    if (!runtime_)
        throw std::runtime_error("failed to create TensorRT runtime");

    engine_.reset(runtime_->deserializeCudaEngine(engine_bytes.data(), engine_bytes.size()));
    if (!engine_)
        throw std::runtime_error("failed to deserialize TensorRT engine");

    context_.reset(engine_->createExecutionContext());
    if (!context_)
        throw std::runtime_error("failed to create TensorRT execution context");

    info_ = {};
    info_.runtime = RuntimeKind::TensorRt;
    info_.active_device = DeviceKind::Gpu;
    info_.active_device_id = config.device_id;
    info_.precision = config.precision;
    info_.provider_name = "TensorRT";

    metadata_ = {};
    metadata_.backend = info_;
    outputs_.clear();
    input_ = {};
    bool has_input = false;

    for (const TrtTensorBinding &binding : collect_bindings(*engine_)) {
        if (binding.input) {
            if (has_input)
                throw std::runtime_error("only single-input TensorRT engines are supported");
            input_ = binding;
            has_input = true;
            metadata_.inputs.push_back(binding.spec);
        } else {
            outputs_.push_back(binding);
            metadata_.outputs.push_back(binding.spec);
        }
    }

    if (!has_input || metadata_.outputs.empty())
        throw std::runtime_error("TensorRT engine must have one input and at least one output");

    return metadata_;
}

std::vector<Tensor> TensorRtBackend::infer(std::span<const Tensor> inputs)
{
    if (!context_ || !engine_)
        throw std::runtime_error("TensorRT backend is not loaded");
    if (inputs.size() != 1)
        throw std::runtime_error("TensorRT backend expects one input tensor");

    std::vector<TrtTensorBinding> bindings;
    bindings.reserve(1 + outputs_.size());
    bindings.push_back(input_);
    bindings.insert(bindings.end(), outputs_.begin(), outputs_.end());

    std::vector<void *> buffers(bindings.size(), nullptr);
    cudaStream_t stream = nullptr;

    try {
        check_cuda(cudaStreamCreate(&stream), "cudaStreamCreate failed");

        for (size_t i = 0; i < bindings.size(); ++i) {
            int64_t count = 1;
            for (int64_t dim : bindings[i].spec.shape)
                count *= dim;
            check_cuda(cudaMalloc(&buffers[i], sizeof(float) * static_cast<size_t>(count)), "cudaMalloc failed");
#if NV_TENSORRT_MAJOR >= 10
            if (!context_->setTensorAddress(bindings[i].name.c_str(), buffers[i]))
                throw std::runtime_error("TensorRT setTensorAddress failed for tensor: " + bindings[i].name);
#endif
        }

        const Tensor &input = inputs[0];
        int64_t input_count = 1;
        for (int64_t dim : input_.spec.shape)
            input_count *= dim;
        if (input.host_data.size() != static_cast<size_t>(input_count))
            throw std::runtime_error("input tensor data size does not match TensorRT binding");

        check_cuda(cudaMemcpyAsync(buffers[0], input.host_data.data(),
                       sizeof(float) * input.host_data.size(), cudaMemcpyHostToDevice, stream),
                   "cudaMemcpyAsync input failed");

#if NV_TENSORRT_MAJOR >= 10
        if (!context_->enqueueV3(stream))
            throw std::runtime_error("TensorRT enqueueV3 failed");
#else
        if (!context_->enqueueV2(buffers.data(), stream, nullptr))
            throw std::runtime_error("TensorRT enqueueV2 failed");
#endif

        std::vector<Tensor> outputs;
        outputs.reserve(outputs_.size());
        for (size_t output_index = 0; output_index < outputs_.size(); ++output_index) {
            const TrtTensorBinding &binding = outputs_[output_index];
            Tensor output;
            output.spec = binding.spec;
            int64_t count = 1;
            for (int64_t dim : binding.spec.shape)
                count *= dim;
            output.host_data.resize(static_cast<size_t>(count));
            check_cuda(cudaMemcpyAsync(output.host_data.data(), buffers[output_index + 1],
                           sizeof(float) * output.host_data.size(), cudaMemcpyDeviceToHost, stream),
                       "cudaMemcpyAsync output failed");
            outputs.push_back(std::move(output));
        }

        check_cuda(cudaStreamSynchronize(stream), "cudaStreamSynchronize failed");
        for (void *buffer : buffers)
            cudaFree(buffer);
        cudaStreamDestroy(stream);
        return outputs;
    } catch (...) {
        for (void *buffer : buffers) {
            if (buffer)
                cudaFree(buffer);
        }
        if (stream)
            cudaStreamDestroy(stream);
        throw;
    }
}

std::unique_ptr<InferenceBackend> create_tensorrt_backend()
{
    return std::make_unique<TensorRtBackend>();
}

} // namespace ssv::infer
