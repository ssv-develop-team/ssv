#include "ssv_inference_backend.hpp"

#include <NvInfer.h>
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
struct TrtDeleter {
    void operator()(T *ptr) const
    {
        if (ptr)
            ptr->destroy();
    }
};

template <typename T>
using TrtPtr = std::unique_ptr<T, TrtDeleter<T>>;

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

int64_t volume(const nvinfer1::Dims &dims)
{
    int64_t result = 1;
    for (int i = 0; i < dims.nbDims; ++i) {
        if (dims.d[i] <= 0)
            throw std::runtime_error("dynamic TensorRT bindings are not supported in this stage");
        result *= dims.d[i];
    }
    return result;
}

std::vector<int64_t> shape_from_dims(const nvinfer1::Dims &dims)
{
    std::vector<int64_t> shape;
    shape.reserve(dims.nbDims);
    for (int i = 0; i < dims.nbDims; ++i)
        shape.push_back(dims.d[i]);
    return shape;
}

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
    int input_binding_ = -1;
    std::vector<int> output_bindings_;
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
    output_bindings_.clear();
    input_binding_ = -1;

    for (int i = 0; i < engine_->getNbBindings(); ++i) {
        TensorSpec spec = spec_from_binding(*engine_, i);
        if (engine_->bindingIsInput(i)) {
            if (input_binding_ != -1)
                throw std::runtime_error("only single-input TensorRT engines are supported");
            input_binding_ = i;
            metadata_.inputs.push_back(spec);
        } else {
            output_bindings_.push_back(i);
            metadata_.outputs.push_back(spec);
        }
    }

    if (input_binding_ == -1 || metadata_.outputs.empty())
        throw std::runtime_error("TensorRT engine must have one input and at least one output");

    return metadata_;
}

std::vector<Tensor> TensorRtBackend::infer(std::span<const Tensor> inputs)
{
    if (!context_ || !engine_)
        throw std::runtime_error("TensorRT backend is not loaded");
    if (inputs.size() != 1)
        throw std::runtime_error("TensorRT backend expects one input tensor");

    int nb_bindings = engine_->getNbBindings();
    std::vector<void *> buffers(nb_bindings, nullptr);
    cudaStream_t stream = nullptr;

    try {
        check_cuda(cudaStreamCreate(&stream), "cudaStreamCreate failed");

        for (int i = 0; i < nb_bindings; ++i) {
            int64_t count = volume(engine_->getBindingDimensions(i));
            check_cuda(cudaMalloc(&buffers[i], sizeof(float) * static_cast<size_t>(count)), "cudaMalloc failed");
        }

        const Tensor &input = inputs[0];
        int64_t input_count = volume(engine_->getBindingDimensions(input_binding_));
        if (input.host_data.size() != static_cast<size_t>(input_count))
            throw std::runtime_error("input tensor data size does not match TensorRT binding");

        check_cuda(cudaMemcpyAsync(buffers[input_binding_], input.host_data.data(),
                       sizeof(float) * input.host_data.size(), cudaMemcpyHostToDevice, stream),
                   "cudaMemcpyAsync input failed");

        if (!context_->enqueueV2(buffers.data(), stream, nullptr))
            throw std::runtime_error("TensorRT enqueueV2 failed");

        std::vector<Tensor> outputs;
        outputs.reserve(output_bindings_.size());
        for (int binding : output_bindings_) {
            Tensor output;
            output.spec = spec_from_binding(*engine_, binding);
            int64_t count = volume(engine_->getBindingDimensions(binding));
            output.host_data.resize(static_cast<size_t>(count));
            check_cuda(cudaMemcpyAsync(output.host_data.data(), buffers[binding],
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
