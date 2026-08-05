#include "backends/onnxruntime/ssv_onnxruntime_backend.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

#include <unistd.h>

#ifndef SSV_ONNXRUNTIME_DEPENDENCY_SIGNATURE
#define SSV_ONNXRUNTIME_DEPENDENCY_SIGNATURE ""
#endif

namespace ssv::infer {

struct SsvOrtSessionState {
    SsvOrtSessionState(
        std::unique_ptr<Ort::Session> value,
        std::filesystem::path profiling_path)
        : session(std::move(value))
        , profiling_directory(std::move(profiling_path))
    {
    }

    ~SsvOrtSessionState()
    {
        session.reset();
        if (!profiling_directory.empty()) {
            std::error_code error;
            std::filesystem::remove_all(profiling_directory, error);
        }
    }

    std::unique_ptr<Ort::Session> session;
    std::filesystem::path profiling_directory;
    std::mutex profiling_mutex;
    std::vector<SsvNodePlacement> node_placements;
    bool profiling_complete = false;
};

namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t elapsed_us(Clock::time_point start)
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now() - start)
            .count());
}

int64_t tensor_size(const std::vector<int64_t> &shape)
{
    if (shape.empty())
        throw std::runtime_error("tensor shape is empty");
    int64_t size = 1;
    for (int64_t dim : shape) {
        if (dim <= 0)
            throw std::runtime_error("dynamic tensor shapes are not supported in this stage");
        if (size > std::numeric_limits<int64_t>::max() / dim)
            throw std::runtime_error("tensor shape element count overflows");
        size *= dim;
    }
    return size;
}

TensorSpec tensor_spec_from_ort(
    const std::string &name,
    const Ort::TypeInfo &type_info,
    bool input)
{
    auto tensor_info = type_info.GetTensorTypeAndShapeInfo();
    const auto element_type = tensor_info.GetElementType();
    if (element_type != ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8
        && element_type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
        throw std::runtime_error(
            "model tensors must use uint8 or float32");
    }
    if (!input && element_type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
        throw std::runtime_error("model output tensors must use float32");

    TensorSpec spec;
    spec.name = name;
    spec.dtype = element_type == ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8
        ? DataType::Uint8
        : DataType::Float32;
    spec.shape = tensor_info.GetShape();
    if (spec.shape.size() == 4 && spec.shape[3] == 4)
        spec.layout = TensorLayout::Nhwc;
    else if (spec.shape.size() == 4 && spec.shape[1] == 3)
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

std::vector<ssv::SsvProvider> provider_chain(
    const SsvProviderAttempt &attempt)
{
    std::vector<ssv::SsvProvider> result;
    result.reserve(attempt.providers.size());
    for (const auto &registration : attempt.providers)
        result.push_back(registration.provider);
    return result;
}

std::optional<ssv::SsvProvider> accelerated_provider(
    const SsvProviderAttempt &attempt)
{
    for (const auto &registration : attempt.providers) {
        if (registration.provider != ssv::SsvProvider::Cpu)
            return registration.provider;
    }
    return std::nullopt;
}

std::string option_value(
    const SsvProviderRegistration &registration,
    std::string_view name)
{
    for (const auto &[key, value] : registration.options) {
        if (key == name)
            return value;
    }
    return {};
}

std::vector<ssv::SsvProvider> available_providers()
{
    std::vector<ssv::SsvProvider> result;
    for (const auto &runtime_name : Ort::GetAvailableProviders()) {
        for (const auto provider : {
                 ssv::SsvProvider::TensorRt,
                 ssv::SsvProvider::Cuda,
                 ssv::SsvProvider::OpenVino,
                 ssv::SsvProvider::MiGraphX,
                 ssv::SsvProvider::Cpu,
             }) {
            if (runtime_name == ssv_provider_runtime_name(provider)) {
                result.push_back(provider);
                break;
            }
        }
    }
    return result;
}

bool supports_native_cache(const SsvProviderAttempt &attempt)
{
    return std::any_of(
        attempt.providers.begin(), attempt.providers.end(),
        [](const auto &registration) {
            return registration.provider == ssv::SsvProvider::TensorRt
                || registration.provider == ssv::SsvProvider::OpenVino
                || registration.provider == ssv::SsvProvider::MiGraphX;
        });
}

std::filesystem::path make_profiling_directory()
{
    auto pattern = (std::filesystem::temp_directory_path()
        / "ssv-ort-profile-XXXXXX")
                       .string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    const char *created = mkdtemp(writable.data());
    if (created == nullptr)
        throw std::runtime_error("failed to create ORT profiling directory");
    return created;
}

void configure_threads(
    Ort::SessionOptions &options,
    const SsvSessionThreading &threading)
{
    options.SetIntraOpNumThreads(threading.intra_op_threads);
    options.SetInterOpNumThreads(threading.inter_op_threads);
    options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    options.AddConfigEntry(
        "session.intra_op.allow_spinning",
        threading.allow_spinning ? "1" : "0");
    options.AddConfigEntry(
        "session.inter_op.allow_spinning",
        threading.allow_spinning ? "1" : "0");
}

void append_provider(
    Ort::SessionOptions &options,
    const SsvProviderRegistration &registration,
    const SsvCacheLocation &cache)
{
    try {
        switch (registration.provider) {
        case ssv::SsvProvider::TensorRt: {
            std::unordered_map<std::string, std::string> values(
                registration.options.begin(), registration.options.end());
            if (!cache.path.empty()) {
                values.emplace("trt_engine_cache_enable", "1");
                values.emplace(
                    "trt_engine_cache_path", cache.path.string());
                values.emplace("trt_timing_cache_enable", "1");
                values.emplace(
                    "trt_timing_cache_path", cache.path.string());
            }
            Ort::TensorRTProviderOptions provider_options;
            provider_options.Update(values);
            options.AppendExecutionProvider_TensorRT_V2(*provider_options);
            break;
        }
        case ssv::SsvProvider::Cuda: {
            std::unordered_map<std::string, std::string> values(
                registration.options.begin(), registration.options.end());
            Ort::CUDAProviderOptions provider_options;
            provider_options.Update(values);
            options.AppendExecutionProvider_CUDA_V2(*provider_options);
            break;
        }
        case ssv::SsvProvider::OpenVino: {
            std::unordered_map<std::string, std::string> values(
                registration.options.begin(), registration.options.end());
            if (!cache.path.empty())
                values.emplace("cache_dir", cache.path.string());
            options.AppendExecutionProvider_OpenVINO_V2(values);
            break;
        }
        case ssv::SsvProvider::MiGraphX: {
            OrtMIGraphXProviderOptions provider_options {};
            provider_options.device_id = std::stoi(
                option_value(registration, "device_id"));
            provider_options.migraphx_fp16_enable = std::stoi(
                option_value(registration, "migraphx_fp16_enable"));
            provider_options.migraphx_mem_limit =
                std::numeric_limits<std::size_t>::max();
            std::string compiled_model;
            if (!cache.path.empty()) {
                compiled_model = (cache.path / "model.mxr").string();
                provider_options.migraphx_save_compiled_model = 1;
                provider_options.migraphx_save_model_path =
                    compiled_model.c_str();
                if (std::filesystem::is_regular_file(compiled_model)) {
                    provider_options.migraphx_load_compiled_model = 1;
                    provider_options.migraphx_load_model_path =
                        compiled_model.c_str();
                }
            }
            options.AppendExecutionProvider_MIGraphX(provider_options);
            break;
        }
        case ssv::SsvProvider::Cpu:
            break;
        }
    } catch (const std::exception &error) {
        throw SsvProviderAttemptError(
            registration.provider,
            SsvProviderFailureStage::Append,
            std::string(ssv_provider_runtime_name(registration.provider))
                + " append failed: " + error.what());
    }
}

std::shared_ptr<SsvOrtSessionState> create_session_state(
    Ort::Env &environment,
    const std::string &model_path,
    const SsvProviderAttempt &attempt,
    const SsvCacheLocation &cache)
{
    auto profiling_directory = make_profiling_directory();
    try {
        Ort::SessionOptions options;
        configure_threads(options, attempt.threading);
        if (attempt.disable_cpu_fallback) {
            options.AddConfigEntry(
                "session.disable_cpu_ep_fallback", "1");
        }
        options.SetGraphOptimizationLevel(
            GraphOptimizationLevel::ORT_ENABLE_ALL);
        const auto profiling_prefix =
            (profiling_directory / "profile").string();
        options.EnableProfiling(profiling_prefix.c_str());
        for (const auto &registration : attempt.providers)
            append_provider(options, registration, cache);
        try {
            auto session = std::make_unique<Ort::Session>(
                environment, model_path.c_str(), options);
            return std::make_shared<SsvOrtSessionState>(
                std::move(session), std::move(profiling_directory));
        } catch (const std::exception &error) {
            throw SsvProviderAttemptError(
                std::nullopt,
                SsvProviderFailureStage::Session,
                "ONNX Runtime session initialization failed: "
                    + std::string(error.what()));
        }
    } catch (...) {
        std::error_code error;
        std::filesystem::remove_all(profiling_directory, error);
        throw;
    }
}

void profile_session_once(
    SsvOrtSessionState &state,
    const TensorSpec &input,
    Ort::MemoryInfo &memory_info,
    const std::vector<const char *> &input_names,
    const std::vector<const char *> &output_names,
    std::vector<Ort::Value> &output_values)
{
    std::lock_guard<std::mutex> lock(state.profiling_mutex);
    if (state.profiling_complete)
        return;

    const auto input_elements = static_cast<std::size_t>(
        tensor_size(input.shape));
    std::vector<std::uint8_t> zero_input(input_elements);
    Ort::Value input_value = Ort::Value::CreateTensor<std::uint8_t>(
        memory_info,
        zero_input.data(),
        zero_input.size(),
        const_cast<int64_t *>(input.shape.data()),
        input.shape.size());
    Ort::RunOptions run_options;
    state.session->Run(
        run_options,
        input_names.data(), &input_value, 1,
        output_names.data(), output_values.data(), output_values.size());

    Ort::AllocatorWithDefaultOptions allocator;
    auto profile_path = state.session->EndProfilingAllocated(allocator);
    if (!profile_path)
        throw std::runtime_error("ONNX Runtime returned no profiling output");
    state.node_placements = ssv_ort_profile_read(profile_path.get());
    state.profiling_complete = true;
    std::error_code error;
    std::filesystem::remove_all(state.profiling_directory, error);
    if (error) {
        throw std::runtime_error(
            "failed to remove ORT profiling output: " + error.message());
    }
    state.profiling_directory.clear();
}

} // namespace

OnnxRuntimeBackend::OnnxRuntimeBackend()
    : profile_(ssv_build_runtime_profile())
    , env_(ORT_LOGGING_LEVEL_WARNING, "ssv-infer")
    , session_pool_(
          std::make_shared<SsvSessionPool<SsvOrtSessionState>>())
    , memory_info_(Ort::MemoryInfo::CreateCpu(
          OrtArenaAllocator, OrtMemTypeDefault))
{
}

BackendInfo OnnxRuntimeBackend::info() const
{
    return info_;
}

ModelMetadata OnnxRuntimeBackend::load(
    const InferenceConfig &config,
    SsvInferenceBufferAllocator &allocator)
{
    const auto total_start = Clock::now();
    info_ = {};
    info_.active_device_id = config.device_id;
    auto &runtime_info = std::get<OnnxRuntimeBackendInfo>(info_.runtime);
    runtime_info.model_hash = ssv_model_sha256(config.model_path);

    const auto detected_device = ssv_detect_device_identity(
        profile_, config.device_id);
    SsvProviderResolveRequest request;
    request.profile = profile_;
    request.providers = config.providers;
    request.precision = config.precision;
    request.device_id = config.device_id;
    request.cpu_threads = config.cpu_threads;
    request.logical_cpu_count = std::thread::hardware_concurrency();
    request.available_providers = available_providers();

    std::shared_ptr<SsvOrtSessionState> selected_session;
    SsvSessionKey selected_key;
    SsvCacheLocation selected_cache;
    std::string selected_device_identity;
    bool selected_pool_hit = false;
    std::uint64_t session_acquire_us = 0;
    const auto resolution_start = Clock::now();
    const auto resolution = ssv_provider_resolve(
        request, [&](const SsvProviderAttempt &attempt) {
            const auto acquire_start = Clock::now();
            try {
                selected_key = {
                    runtime_info.model_hash,
                    provider_chain(attempt),
                    config.device_id,
                    attempt.precision,
                };
                const bool native_cache = supports_native_cache(attempt);
                const bool accelerated = accelerated_provider(attempt).has_value();
                selected_device_identity = accelerated
                    ? detected_device.value
                    : "cpu";
                SsvCacheIdentity cache_identity;
                cache_identity.session_key = selected_key;
                cache_identity.provider_options = attempt.providers;
                cache_identity.dependency_signature =
                    SSV_ONNXRUNTIME_DEPENDENCY_SIGNATURE;
                cache_identity.device_identity = selected_device_identity;
                cache_identity.runtime_versions = {
                    {"onnxruntime", Ort::GetVersionString()},
                    {"profile", std::string(
                        ssv_runtime_profile_name(profile_))},
                };
                try {
                    selected_cache = ssv_cache_prepare(
                        config.cache, cache_identity, native_cache);
                } catch (const std::exception &error) {
                    throw SsvProviderAttemptError(
                        accelerated_provider(attempt),
                        SsvProviderFailureStage::Cache,
                        error.what());
                }

                auto acquire = [&] {
                    return session_pool_->acquire(selected_key, [&] {
                        return create_session_state(
                            env_, config.model_path, attempt, selected_cache);
                    });
                };
                try {
                    auto result = acquire();
                    selected_session = std::move(result.session);
                    selected_pool_hit = result.reused;
                } catch (const SsvProviderAttemptError &error) {
                    if (error.stage() != SsvProviderFailureStage::Session
                        || selected_cache.status != SsvCacheStatus::Hit) {
                        throw;
                    }
                    selected_cache = ssv_cache_rebuild(
                        config.cache, cache_identity);
                    auto result = acquire();
                    selected_session = std::move(result.session);
                    selected_pool_hit = result.reused;
                }
            } catch (...) {
                session_acquire_us += elapsed_us(acquire_start);
                throw;
            }
            session_acquire_us += elapsed_us(acquire_start);
        });
    const auto resolution_total_us = elapsed_us(resolution_start);

    if (!selected_session)
        throw std::logic_error("Provider resolver returned no ORT session");
    session_state_ = std::move(selected_session);
    info_.resolved_precision = resolution.active.precision;
    runtime_info.active_provider_chain = provider_chain(resolution.active);
    runtime_info.fallbacks = resolution.fallbacks;
    runtime_info.intra_op_threads =
        resolution.active.threading.intra_op_threads;
    runtime_info.inter_op_threads =
        resolution.active.threading.inter_op_threads;
    runtime_info.allow_spinning =
        resolution.active.threading.allow_spinning;
    runtime_info.session_key = ssv_session_key_id(selected_key);
    runtime_info.session_pool_hit = selected_pool_hit;
    runtime_info.cache_status = selected_cache.status;
    runtime_info.cache_namespace = selected_cache.namespace_id;
    runtime_info.device_identity = selected_device_identity;
    if (selected_cache.status == SsvCacheStatus::Unavailable) {
        runtime_info.cache_reason =
            std::string_view(SSV_ONNXRUNTIME_DEPENDENCY_SIGNATURE).empty()
            ? "dependency snapshot signature is unavailable"
            : detected_device.unavailable_reason;
    }
    runtime_info.startup_timings.session_acquire_us = session_acquire_us;
    runtime_info.startup_timings.provider_resolution_us =
        resolution_total_us > session_acquire_us
        ? resolution_total_us - session_acquire_us
        : 0;

    const auto metadata_start = Clock::now();
    Ort::AllocatorWithDefaultOptions alloc;
    input_names_.clear();
    output_names_.clear();
    input_name_ptrs_.clear();
    output_name_ptrs_.clear();
    metadata_ = {};

    const auto input_count = session_state_->session->GetInputCount();
    for (std::size_t index = 0; index < input_count; ++index) {
        auto name = session_state_->session->GetInputNameAllocated(index, alloc);
        input_names_.push_back(name.get());
        metadata_.inputs.push_back(tensor_spec_from_ort(
            input_names_.back(),
            session_state_->session->GetInputTypeInfo(index),
            true));
    }

    const auto output_count = session_state_->session->GetOutputCount();
    for (std::size_t index = 0; index < output_count; ++index) {
        auto name = session_state_->session->GetOutputNameAllocated(index, alloc);
        output_names_.push_back(name.get());
        metadata_.outputs.push_back(tensor_spec_from_ort(
            output_names_.back(),
            session_state_->session->GetOutputTypeInfo(index),
            false));
    }
    input_name_ptrs_ = c_names(input_names_);
    output_name_ptrs_ = c_names(output_names_);
    if (metadata_.inputs.size() != 1 || metadata_.outputs.empty()) {
        throw std::runtime_error(
            "model must have one input and at least one output");
    }

    auto model_metadata = session_state_->session->GetModelMetadata();
    for (auto &allocated_key :
         model_metadata.GetCustomMetadataMapKeysAllocated(alloc)) {
        if (!allocated_key)
            continue;
        const std::string key = allocated_key.get();
        auto allocated_value =
            model_metadata.LookupCustomMetadataMapAllocated(key.c_str(), alloc);
        if (allocated_value)
            metadata_.properties.emplace(key, allocated_value.get());
    }

    output_buffers_.clear();
    output_values_.clear();
    output_views_.clear();
    output_buffers_.reserve(metadata_.outputs.size());
    output_values_.reserve(metadata_.outputs.size());
    output_views_.reserve(metadata_.outputs.size());
    for (const TensorSpec &output : metadata_.outputs) {
        const auto element_count = static_cast<std::size_t>(
            tensor_size(output.shape));
        output_buffers_.push_back(allocator.allocate(
            element_count * sizeof(float), alignof(float)));
        auto host_data = output_buffers_.back().as_span<float>();
        output_values_.push_back(Ort::Value::CreateTensor<float>(
            memory_info_,
            host_data.data(),
            host_data.size(),
            const_cast<int64_t *>(output.shape.data()),
            output.shape.size()));
    }
    for (std::size_t index = 0; index < metadata_.outputs.size(); ++index) {
        output_views_.push_back({
            &metadata_.outputs[index],
            output_buffers_[index].as_span<float>(),
        });
    }
    runtime_info.startup_timings.metadata_read_us = elapsed_us(metadata_start);
    runtime_info.startup_timings.total_us = elapsed_us(total_start);
    metadata_.backend = info_;
    return metadata_;
}

void OnnxRuntimeBackend::warmup()
{
    if (!session_state_ || metadata_.inputs.size() != 1)
        throw std::runtime_error("ONNX Runtime backend is not loaded");

    const auto profiling_start = Clock::now();
    profile_session_once(
        *session_state_,
        metadata_.inputs.front(),
        memory_info_,
        input_name_ptrs_,
        output_name_ptrs_,
        output_values_);

    auto &runtime_info = std::get<OnnxRuntimeBackendInfo>(info_.runtime);
    runtime_info.startup_timings.profiling_warmup_us =
        elapsed_us(profiling_start);
    runtime_info.startup_timings.total_us +=
        runtime_info.startup_timings.profiling_warmup_us;
    runtime_info.node_placements = session_state_->node_placements;
    metadata_.backend = info_;
}

std::span<const SsvFloatTensorView> OnnxRuntimeBackend::infer(
    const SsvUint8TensorView &input,
    std::stop_token stop_token)
{
    if (!session_state_ || !session_state_->session)
        throw std::runtime_error("ONNX Runtime backend is not loaded");
    if (input_names_.size() != 1 || input.spec == nullptr)
        throw std::runtime_error("ONNX Runtime backend expects one input");
    const int64_t expected_size = tensor_size(input.spec->shape);
    if (expected_size != static_cast<int64_t>(input.host_data.size()))
        throw std::runtime_error("input tensor data size does not match shape");

    Ort::Value input_value = Ort::Value::CreateTensor<std::uint8_t>(
        memory_info_,
        const_cast<std::uint8_t *>(input.host_data.data()),
        input.host_data.size(),
        const_cast<int64_t *>(input.spec->shape.data()),
        input.spec->shape.size());

    run_options_.UnsetTerminate();
    try {
        {
            std::stop_callback cancel_run(
                stop_token, [this] { terminate_run(); });
            if (stop_token.stop_requested())
                throw std::runtime_error("ONNX Runtime inference cancelled");
            session_state_->session->Run(
                run_options_,
                input_name_ptrs_.data(), &input_value, 1,
                output_name_ptrs_.data(),
                output_values_.data(), output_values_.size());
        }
    } catch (...) {
        try {
            run_options_.UnsetTerminate();
        } catch (...) {
        }
        throw;
    }
    run_options_.UnsetTerminate();
    return output_views_;
}

void OnnxRuntimeBackend::terminate_run() noexcept
{
    try {
        run_options_.SetTerminate();
    } catch (...) {
    }
}

std::unique_ptr<InferenceBackend> create_onnxruntime_backend()
{
    return std::make_unique<OnnxRuntimeBackend>();
}

} // namespace ssv::infer
