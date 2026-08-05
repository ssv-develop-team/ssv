#include "core/ssv_inference_backend.hpp"
#include "backends/tensorrt/ssv_tensorrt_manifest.hpp"
#include "backends/tensorrt/ssv_tensorrt_resources.hpp"

#include <NvInfer.h>
#include <NvInferVersion.h>
#include <cuda_runtime_api.h>

#include <cstddef>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
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

std::string tensorrt_version()
{
    return std::to_string(NV_TENSORRT_MAJOR) + "."
        + std::to_string(NV_TENSORRT_MINOR) + "."
        + std::to_string(NV_TENSORRT_PATCH) + "."
        + std::to_string(NV_TENSORRT_BUILD);
}

std::size_t binding_bytes(const TensorSpec &spec)
{
    std::size_t count = 1;
    for (const auto dimension : spec.shape) {
        if (dimension <= 0) {
            throw std::runtime_error(
                "dynamic TensorRT shapes are not supported");
        }
        const auto value = static_cast<std::size_t>(dimension);
        if (count > std::numeric_limits<std::size_t>::max() / value)
            throw std::runtime_error("TensorRT tensor size overflows size_t");
        count *= value;
    }
    const std::size_t element_size = spec.dtype == DataType::Uint8
        ? sizeof(std::uint8_t)
        : sizeof(float);
    if (count > std::numeric_limits<std::size_t>::max() / element_size)
        throw std::runtime_error("TensorRT tensor byte size overflows size_t");
    return count * element_size;
}

cudaStream_t cuda_stream(void *stream)
{
    return reinterpret_cast<cudaStream_t>(stream);
}

class CudaRuntimeApi final : public SsvTensorRtCudaApi {
public:
    SsvTensorRtCudaDeviceInfo select_device(int device_id) override
    {
        activate_device(device_id);
        int runtime_version = 0;
        check_cuda(cudaRuntimeGetVersion(&runtime_version),
            "cudaRuntimeGetVersion failed");
        cudaDeviceProp properties {};
        check_cuda(cudaGetDeviceProperties(&properties, device_id),
            "cudaGetDeviceProperties failed");
        return {
            .cuda_runtime_version = runtime_version,
            .compute_capability_major = properties.major,
            .compute_capability_minor = properties.minor,
        };
    }

    void activate_device(int device_id) override
    {
        check_cuda(cudaSetDevice(device_id), "cudaSetDevice failed");
    }

    void *create_stream() override
    {
        cudaStream_t stream = nullptr;
        check_cuda(cudaStreamCreate(&stream), "cudaStreamCreate failed");
        return reinterpret_cast<void *>(stream);
    }

    void destroy_stream(void *stream) noexcept override
    {
        static_cast<void>(cudaStreamDestroy(cuda_stream(stream)));
    }

    void *allocate_device(std::size_t bytes) override
    {
        void *device = nullptr;
        check_cuda(cudaMalloc(&device, bytes), "cudaMalloc failed");
        return device;
    }

    void free_device(void *device) noexcept override
    {
        static_cast<void>(cudaFree(device));
    }

    void copy_host_to_device(
        void *device,
        const void *host,
        std::size_t bytes,
        void *stream) override
    {
        check_cuda(cudaMemcpyAsync(device,
                       host,
                       bytes,
                       cudaMemcpyHostToDevice,
                       cuda_stream(stream)),
            "cudaMemcpyAsync input failed");
    }

    void copy_device_to_host(
        void *host,
        const void *device,
        std::size_t bytes,
        void *stream) override
    {
        check_cuda(cudaMemcpyAsync(host,
                       device,
                       bytes,
                       cudaMemcpyDeviceToHost,
                       cuda_stream(stream)),
            "cudaMemcpyAsync output failed");
    }

    void synchronize(void *stream) override
    {
        check_cuda(cudaStreamSynchronize(cuda_stream(stream)),
            "cudaStreamSynchronize failed");
    }
};

std::vector<int64_t> shape_from_dims(const nvinfer1::Dims &dims)
{
    std::vector<int64_t> shape;
    shape.reserve(dims.nbDims);
    for (int i = 0; i < dims.nbDims; ++i)
        shape.push_back(dims.d[i]);
    return shape;
}

#if NV_TENSORRT_MAJOR >= 10
TensorSpec spec_from_tensor(
    nvinfer1::ICudaEngine &engine,
    const char *name,
    bool input)
{
    const auto data_type = engine.getTensorDataType(name);
    if ((!input || data_type != nvinfer1::DataType::kUINT8)
        && data_type != nvinfer1::DataType::kFLOAT) {
        throw std::runtime_error("unsupported TensorRT binding data type");
    }
    TensorSpec spec;
    spec.name = name;
    spec.dtype = data_type == nvinfer1::DataType::kUINT8
        ? DataType::Uint8
        : DataType::Float32;
    spec.shape = shape_from_dims(engine.getTensorShape(name));
    if (spec.shape.size() == 4 && spec.shape[3] == 4)
        spec.layout = TensorLayout::Nhwc;
    else if (spec.shape.size() == 4 && spec.shape[1] == 3)
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
        const bool input = mode == nvinfer1::TensorIOMode::kINPUT;
        bindings.push_back({name, spec_from_tensor(engine, name, input), input});
    }
    return bindings;
}
#else
TensorSpec spec_from_binding(
    nvinfer1::ICudaEngine &engine,
    int binding,
    bool input)
{
    const auto data_type = engine.getBindingDataType(binding);
    if ((!input || data_type != nvinfer1::DataType::kUINT8)
        && data_type != nvinfer1::DataType::kFLOAT) {
        throw std::runtime_error("unsupported TensorRT binding data type");
    }
    TensorSpec spec;
    spec.name = engine.getBindingName(binding);
    spec.dtype = data_type == nvinfer1::DataType::kUINT8
        ? DataType::Uint8
        : DataType::Float32;
    spec.shape = shape_from_dims(engine.getBindingDimensions(binding));
    if (spec.shape.size() == 4 && spec.shape[3] == 4)
        spec.layout = TensorLayout::Nhwc;
    else if (spec.shape.size() == 4 && spec.shape[1] == 3)
        spec.layout = TensorLayout::Nchw;
    return spec;
}

std::vector<TrtTensorBinding> collect_bindings(nvinfer1::ICudaEngine &engine)
{
    std::vector<TrtTensorBinding> bindings;
    int count = engine.getNbBindings();
    bindings.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        const bool input = engine.bindingIsInput(i);
        bindings.push_back({
            engine.getBindingName(i),
            spec_from_binding(engine, i, input),
            input,
        });
    }
    return bindings;
}
#endif

} // namespace

class TensorRtBackend final : public InferenceBackend {
public:
    explicit TensorRtBackend(std::unique_ptr<SsvTensorRtCudaApi> cuda)
        : resources_(std::move(cuda))
    {
    }

    BackendInfo info() const override { return info_; }
    ModelMetadata load(
        const InferenceConfig &config,
        SsvInferenceBufferAllocator &allocator) override;
    std::span<const SsvFloatTensorView> infer(
        const SsvUint8TensorView &input,
        std::stop_token stop_token) override;

private:
    TrtLogger logger_;
    TrtPtr<nvinfer1::IRuntime> runtime_{nullptr};
    TrtPtr<nvinfer1::ICudaEngine> engine_{nullptr};
    TrtPtr<nvinfer1::IExecutionContext> context_{nullptr};
    SsvTensorRtExecutionResources resources_;
    BackendInfo info_;
    ModelMetadata metadata_;
    std::vector<TrtTensorBinding> bindings_;
    std::size_t input_binding_index_ =
        std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> output_binding_indices_;
    std::vector<SsvInferenceBuffer> output_buffers_;
    std::vector<SsvFloatTensorView> output_views_;
};

ModelMetadata TensorRtBackend::load(
    const InferenceConfig &config,
    SsvInferenceBufferAllocator &allocator)
{
    if (runtime_ || engine_ || context_)
        throw std::logic_error("TensorRT backend is already loaded");
    if (!config.model_manifest) {
        throw std::invalid_argument(
            "TensorRT backend requires an engine manifest");
    }

    const auto cuda_device = resources_.start(config.device_id);
    if (getInferLibVersion() != NV_TENSORRT_VERSION) {
        throw std::runtime_error(
            "TensorRT headers and runtime library versions do not match");
    }

    std::vector<char> engine_bytes = read_file(config.model_path);
    const SsvTensorRtRuntimeDescriptor runtime_descriptor {
        .tensorrt_version = tensorrt_version(),
        .cuda_runtime_version = cuda_device.cuda_runtime_version,
        .compute_capability_major =
            cuda_device.compute_capability_major,
        .compute_capability_minor =
            cuda_device.compute_capability_minor,
    };
    const auto manifest = ssv_tensorrt_manifest_load_and_validate(
        *config.model_manifest,
        std::as_bytes(std::span(engine_bytes)),
        runtime_descriptor);

    runtime_.reset(nvinfer1::createInferRuntime(logger_));
    if (!runtime_)
        throw std::runtime_error("failed to create TensorRT runtime");

    engine_.reset(runtime_->deserializeCudaEngine(engine_bytes.data(), engine_bytes.size()));
    if (!engine_)
        throw std::runtime_error("failed to deserialize TensorRT engine");

    context_.reset(engine_->createExecutionContext());
    if (!context_)
        throw std::runtime_error("failed to create TensorRT execution context");

    metadata_ = {};
    bindings_ = collect_bindings(*engine_);
    input_binding_index_ = std::numeric_limits<std::size_t>::max();
    output_binding_indices_.clear();
    for (std::size_t index = 0; index < bindings_.size(); ++index) {
        const auto &binding = bindings_[index];
        if (binding.input) {
            if (input_binding_index_
                != std::numeric_limits<std::size_t>::max()) {
                throw std::runtime_error("only single-input TensorRT engines are supported");
            }
            input_binding_index_ = index;
            metadata_.inputs.push_back(binding.spec);
        } else {
            output_binding_indices_.push_back(index);
            metadata_.outputs.push_back(binding.spec);
        }
    }

    if (input_binding_index_ == std::numeric_limits<std::size_t>::max()
        || metadata_.outputs.empty()) {
        throw std::runtime_error("TensorRT engine must have one input and at least one output");
    }
    ssv_tensorrt_manifest_apply(manifest, metadata_);

    output_buffers_.clear();
    output_views_.clear();
    output_buffers_.reserve(output_binding_indices_.size());
    output_views_.reserve(output_binding_indices_.size());
    for (const auto binding_index : output_binding_indices_) {
        const auto &binding = bindings_[binding_index];
        if (binding.spec.dtype != DataType::Float32)
            throw std::runtime_error("TensorRT outputs must use float32");
        output_buffers_.push_back(allocator.allocate(
            binding_bytes(binding.spec), alignof(float)));
    }
    for (std::size_t index = 0;
         index < output_binding_indices_.size();
         ++index) {
        output_views_.push_back({
            &metadata_.outputs[index],
            output_buffers_[index].as_span<float>(),
        });
    }

    std::vector<std::size_t> device_buffer_sizes;
    device_buffer_sizes.reserve(bindings_.size());
    for (const auto &binding : bindings_)
        device_buffer_sizes.push_back(binding_bytes(binding.spec));
    resources_.allocate(device_buffer_sizes);

#if NV_TENSORRT_MAJOR >= 10
    const auto device_buffers = resources_.device_buffers();
    for (std::size_t index = 0; index < bindings_.size(); ++index) {
        if (!context_->setTensorAddress(
                bindings_[index].name.c_str(), device_buffers[index])) {
            throw std::runtime_error(
                "TensorRT setTensorAddress failed for tensor: "
                + bindings_[index].name);
        }
    }
#endif

    info_ = {};
    info_.runtime = TensorRtEngineBackendInfo {
        .engine_hash = manifest.engine_sha256,
        .wrapper_hash = manifest.wrapper_sha256,
        .tensorrt_version = manifest.tensorrt_version,
        .cuda_runtime_version = manifest.cuda_runtime_version,
        .compute_capability_major =
            manifest.compute_capability_major,
        .compute_capability_minor =
            manifest.compute_capability_minor,
    };
    info_.active_device_id = config.device_id;
    info_.resolved_precision = manifest.precision;
    metadata_.backend = info_;

    return metadata_;
}

std::span<const SsvFloatTensorView> TensorRtBackend::infer(
    const SsvUint8TensorView &input,
    std::stop_token stop_token)
{
    if (stop_token.stop_requested())
        throw std::runtime_error("TensorRT inference cancelled");
    if (!context_ || !engine_)
        throw std::runtime_error("TensorRT backend is not loaded");
    void *stream = resources_.activate_stream();
    const auto &input_binding = bindings_[input_binding_index_];
    if (input.spec == nullptr
        || input.spec->name != input_binding.spec.name
        || input.spec->dtype != input_binding.spec.dtype
        || input.spec->shape != input_binding.spec.shape
        || input.spec->layout != input_binding.spec.layout) {
        throw std::runtime_error("TensorRT backend expects one uint8 input tensor");
    }
    resources_.copy_input(input_binding_index_, input.host_data);
    if (stop_token.stop_requested())
        throw std::runtime_error("TensorRT inference cancelled");

#if NV_TENSORRT_MAJOR >= 10
    if (!context_->enqueueV3(cuda_stream(stream)))
        throw std::runtime_error("TensorRT enqueueV3 failed");
#else
    const auto device_buffers = resources_.device_buffers();
    if (!context_->enqueueV2(device_buffers.data(),
            cuda_stream(stream), nullptr)) {
        throw std::runtime_error("TensorRT enqueueV2 failed");
    }
#endif

    for (std::size_t output_index = 0;
         output_index < output_binding_indices_.size();
         ++output_index) {
        auto host_output = output_buffers_[output_index].as_span<float>();
        resources_.copy_output(output_binding_indices_[output_index],
            std::as_writable_bytes(host_output));
    }
    resources_.synchronize();
    if (stop_token.stop_requested())
        throw std::runtime_error("TensorRT inference cancelled");
    return output_views_;
}

std::unique_ptr<InferenceBackend> create_tensorrt_backend()
{
    return std::make_unique<TensorRtBackend>(
        std::make_unique<CudaRuntimeApi>());
}

} // namespace ssv::infer
