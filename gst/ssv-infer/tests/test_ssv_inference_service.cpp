#include "core/ssv_inference_service_internal.hpp"
#include "model/ssv_model_contract_internal.hpp"
#include "core/ssv_inference_backend.hpp"
#include "core/ssv_inference_buffer.hpp"
#include "core/ssv_inference_config.hpp"
#include "ssv_inference_service.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory()
    {
        char path[] = "/tmp/ssv-inference-service-test-XXXXXX";
        const char *created = mkdtemp(path);
        assert(created != nullptr);
        path_ = created;
    }

    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory &) = delete;
    TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

    [[nodiscard]] const std::filesystem::path &path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

ssv::infer::ModelMetadata make_wrapper_metadata(
    std::string output_format = "yolov8")
{
    ssv::infer::ModelMetadata metadata;
    ssv::infer::TensorSpec input;
    input.name = "images_rgba";
    input.dtype = ssv::infer::DataType::Uint8;
    input.shape = {1, 2, 3, 4};
    input.layout = ssv::infer::TensorLayout::Nhwc;
    metadata.inputs.push_back(std::move(input));

    ssv::infer::TensorSpec output;
    output.name = "output0";
    output.dtype = ssv::infer::DataType::Float32;
    output.shape = {1, 1, 6};
    metadata.outputs.push_back(std::move(output));

    metadata.properties = {
        {"ssv.wrapper.channel_rule", "drop_alpha_keep_rgb"},
        {"ssv.wrapper.contract", "rgba_u8_nhwc_v1"},
        {"ssv.wrapper.dtype", "uint8"},
        {"ssv.wrapper.height", "2"},
        {"ssv.wrapper.layout", "NHWC"},
        {"ssv.wrapper.model_family", "yolo"},
        {"ssv.wrapper.normalization", "divide_by_255"},
        {"ssv.wrapper.output_format", std::move(output_format)},
        {"ssv.wrapper.source_sha256", std::string(64, 'a')},
        {"ssv.wrapper.tool", "ssv.prepare_wrapper"},
        {"ssv.wrapper.tool_version", "1.0.0"},
        {"ssv.wrapper.width", "3"},
    };
    return metadata;
}

class CountingAllocator final
    : public ssv::infer::SsvInferenceBufferAllocator {
public:
    ssv::infer::SsvInferenceBuffer allocate(
        std::size_t bytes,
        std::size_t alignment) override
    {
        ++allocation_count;
        return delegate_.allocate(bytes, alignment);
    }

    std::size_t allocation_count = 0;

private:
    ssv::infer::SsvDefaultInferenceBufferAllocator delegate_;
};

class FakeBackend final : public ssv::infer::InferenceBackend {
public:
    explicit FakeBackend(ssv::infer::BackendInfo info = {})
        : info_(std::move(info))
    {
    }

    ssv::infer::BackendInfo info() const override { return info_; }

    ssv::infer::ModelMetadata load(
        const ssv::infer::InferenceConfig &,
        ssv::infer::SsvInferenceBufferAllocator &allocator) override
    {
        metadata_ = make_wrapper_metadata("yolo_nx6");
        output_buffer_ = allocator.allocate(
            6 * sizeof(float), alignof(float));
        output_view_.spec = &metadata_.outputs.front();
        output_view_.host_data = output_buffer_.as_span<float>();
        return metadata_;
    }

    std::span<const ssv::infer::SsvFloatTensorView> infer(
        const ssv::infer::SsvUint8TensorView &input,
        std::stop_token) override
    {
        assert(input.host_data.size() == 24);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        auto output = output_buffer_.as_span<float>();
        const std::array<float, 6> detection = {
            0.1F, 0.2F, 0.4F, 0.8F, 0.9F, 0.0F};
        std::copy(detection.begin(), detection.end(), output.begin());
        return std::span<const ssv::infer::SsvFloatTensorView>(
            &output_view_, 1);
    }

private:
    ssv::infer::BackendInfo info_;
    ssv::infer::ModelMetadata metadata_;
    ssv::infer::SsvInferenceBuffer output_buffer_;
    ssv::infer::SsvFloatTensorView output_view_;
};

class RawModelBackend final : public ssv::infer::InferenceBackend {
public:
    ssv::infer::BackendInfo info() const override { return {}; }

    ssv::infer::ModelMetadata load(
        const ssv::infer::InferenceConfig &,
        ssv::infer::SsvInferenceBufferAllocator &) override
    {
        auto metadata = make_wrapper_metadata();
        metadata.properties.clear();
        return metadata;
    }

    std::span<const ssv::infer::SsvFloatTensorView> infer(
        const ssv::infer::SsvUint8TensorView &,
        std::stop_token) override
    {
        return {};
    }
};

class ControlledBackend final : public ssv::infer::InferenceBackend {
public:
    ssv::infer::BackendInfo info() const override { return {}; }

    ssv::infer::ModelMetadata load(
        const ssv::infer::InferenceConfig &,
        ssv::infer::SsvInferenceBufferAllocator &allocator) override
    {
        metadata_ = make_wrapper_metadata("yolo_nx6");
        output_buffer_ = allocator.allocate(
            6 * sizeof(float), alignof(float));
        output_view_.spec = &metadata_.outputs.front();
        output_view_.host_data = output_buffer_.as_span<float>();
        return metadata_;
    }

    std::span<const ssv::infer::SsvFloatTensorView> infer(
        const ssv::infer::SsvUint8TensorView &,
        std::stop_token stop_token) override
    {
        std::unique_lock<std::mutex> lock(mutex_);
        ++entered_count_;
        const auto call_index = entered_count_;
        ++active_count_;
        max_active_count_ = std::max(max_active_count_, active_count_);
        condition_.notify_all();
        std::stop_callback wake_on_stop(
            stop_token, [this] { condition_.notify_all(); });
        condition_.wait(lock, [this, &stop_token] {
            return completion_permits_ > 0
                || stop_token.stop_requested();
        });
        const bool cancelled = stop_token.stop_requested();
        if (!cancelled)
            --completion_permits_;
        --active_count_;
        const bool should_fail = failure_call_ == call_index;
        lock.unlock();

        if (cancelled)
            throw std::runtime_error("controlled inference cancelled");
        if (should_fail)
            throw std::runtime_error("controlled inference failure");

        auto output = output_buffer_.as_span<float>();
        const std::array<float, 6> detection = {
            0.1F, 0.2F, 0.4F, 0.8F, 0.9F, 0.0F};
        std::copy(detection.begin(), detection.end(), output.begin());
        return std::span<const ssv::infer::SsvFloatTensorView>(
            &output_view_, 1);
    }

    bool wait_until_entered(std::size_t expected)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(2),
            [this, expected] { return entered_count_ >= expected; });
    }

    void allow_one_completion()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++completion_permits_;
        condition_.notify_all();
    }

    void fail_on_call(std::size_t call_index)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        failure_call_ = call_index;
    }

    std::size_t max_active_count() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return max_active_count_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::size_t entered_count_ = 0;
    std::size_t active_count_ = 0;
    std::size_t max_active_count_ = 0;
    std::size_t completion_permits_ = 0;
    std::size_t failure_call_ = 0;
    ssv::infer::ModelMetadata metadata_;
    ssv::infer::SsvInferenceBuffer output_buffer_;
    ssv::infer::SsvFloatTensorView output_view_;
};

ssv::SsvInferenceConfig make_inference_config()
{
    const char *label_map = std::getenv("SSV_TEST_LABEL_MAP_PATH");
    assert(label_map != nullptr && label_map[0] != '\0');
    ssv::SsvInferenceConfig config;
    config.model.path = "/bin/true";
    config.model.family = "yolo";
    config.model.output_format = "yolo_nx6";
    config.model.label_map = label_map;
    config.target_class = "person";
    return config;
}

ssv::infer::SsvInferenceRequest make_request(
    SsvInferenceService *service,
    std::uint64_t frame_id,
    std::string_view source_id = "camera-01",
    bool padded = false)
{
    GstVideoInfo info;
    gst_video_info_init(&info);
    assert(gst_video_info_set_format(&info, GST_VIDEO_FORMAT_RGBA, 3, 2));
    if (padded) {
        info.stride[0] = 16;
        info.size = 32;
    }
    GstBuffer *buffer = gst_buffer_new_allocate(
        nullptr, padded ? 32 : 24, nullptr);
    assert(buffer != nullptr);

    ssv::infer::SsvInferenceRequest request;
    request.frame_id = frame_id;
    request.source_id = source_id;
    request.analysis_frame =
        ssv::infer::ssv_inference_service_create_analysis_frame(
            service,
            buffer,
            info,
            {3, 2, 3, 2, 1.0F, 0, 0, 0, 0},
            {frame_id * GST_SECOND, GST_SECOND / 15, 7});
    gst_buffer_unref(buffer);
    return request;
}

bool wait_for_pending(
    SsvInferenceService *service,
    std::uint64_t expected)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (ssv::infer::ssv_inference_service_stats(service).pending
            == expected) {
            return true;
        }
        std::this_thread::yield();
    }
    return false;
}

bool wait_for_staging_leases(
    SsvInferenceService *service,
    std::size_t expected)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (ssv::infer::ssv_inference_service_stats(service)
                .analysis_frames.outstanding_staging_leases
            == expected) {
            return true;
        }
        std::this_thread::yield();
    }
    return false;
}

bool wait_for_active_maps(
    SsvInferenceService *service,
    std::size_t expected)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (ssv::infer::ssv_inference_service_stats(service)
                .analysis_frames.active_maps
            == expected) {
            return true;
        }
        std::this_thread::yield();
    }
    return false;
}

void test_inference_stats_summarizes_a_fixed_window()
{
    ssv::infer::SsvInferenceStatsWindowInput input;
    input.started_at_us = 1'000'000;
    input.ended_at_us = 6'000'000;
    input.previous_completion_us = 750'000;
    input.received = 5;
    input.dropped = 2;
    input.completed_samples = {
        {2'000'000, {10, 100, 0, 40, 160}},
        {3'000'000, {30, 300, 0, 60, 390}},
        {5'500'000, {20, 200, 0, 50, 270}},
    };

    const auto stats = ssv::infer::ssv_inference_stats_summarize(input);

    assert(stats.received == 5);
    assert(stats.dropped == 2);
    assert(stats.completed == 3);
    assert(stats.completed_fps == 0.6);
    assert(stats.longest_result_gap_us == 2'500'000);
    assert(stats.queue.p50_us == 20);
    assert(stats.queue.p95_us == 30);
    assert(stats.device.p50_us == 200);
    assert(stats.device.p95_us == 300);
    assert(stats.output_copy.p50_us == 0);
    assert(stats.output_copy.p95_us == 0);
    assert(stats.postprocess.p50_us == 50);
    assert(stats.postprocess.p95_us == 60);
    assert(stats.total.p50_us == 270);
    assert(stats.total.p95_us == 390);
}

void test_model_contract_accepts_only_wrapper_input()
{
    auto metadata = make_wrapper_metadata();
    const auto contract = ssv::infer::ssv_model_contract_validate(
        metadata,
        ssv::infer::ModelFamily::Yolo,
        ssv::infer::OutputFormat::YoloV8);
    assert(contract.width == 3);
    assert(contract.height == 2);
    assert(contract.input_bytes == 24);

    metadata.properties.clear();
    try {
        static_cast<void>(ssv::infer::ssv_model_contract_validate(
            metadata,
            ssv::infer::ModelFamily::Yolo,
            ssv::infer::OutputFormat::YoloV8));
        assert(false && "raw model was accepted as an SSV wrapper");
    } catch (const ssv::infer::SsvModelContractError &error) {
        assert(std::string(error.what()).find("ssv.wrapper.contract")
            != std::string::npos);
    }
}

void test_service_projects_onnx_runtime_snapshot()
{
    ssv::infer::BackendInfo backend;
    auto &onnx = std::get<ssv::infer::OnnxRuntimeBackendInfo>(
        backend.runtime);
    onnx.active_provider_chain = {
        ssv::SsvProvider::Cuda, ssv::SsvProvider::Cpu};
    onnx.fallbacks = {
        {
            .provider = ssv::SsvProvider::TensorRt,
            .stage = ssv::infer::SsvProviderFailureStage::Availability,
            .reason = "TensorRT is unavailable",
        },
        {
            .provider = ssv::SsvProvider::Cuda,
            .stage = ssv::infer::SsvProviderFailureStage::Session,
            .reason = "CUDA session creation failed",
        },
    };
    onnx.model_hash = std::string(64, 'd');
    onnx.cache_status = ssv::infer::SsvCacheStatus::Hit;
    onnx.device_identity = "pci:0000:01:00.0";
    backend.resolved_precision = ssv::SsvPrecision::Fp16;

    auto service = ssv::infer::ssv_inference_service_create_with_backend(
        make_inference_config(),
        std::make_unique<FakeBackend>(std::move(backend)));
    const auto snapshot =
        ssv::infer::ssv_inference_service_runtime_snapshot(service.get());

    assert(snapshot.provider_chain
        == "CUDAExecutionProvider,CPUExecutionProvider");
    assert(snapshot.provider_device == "pci:0000:01:00.0");
    assert(snapshot.precision == "fp16");
    assert(snapshot.model_hash == std::string(64, 'd'));
    assert(snapshot.input_contract == "rgba_u8_nhwc_v1");
    assert(snapshot.cache_status == "hit");
    assert(snapshot.fallbacks.size() == 2);
    assert(snapshot.fallbacks[0].provider
        == "TensorrtExecutionProvider");
    assert(snapshot.fallbacks[0].resolved_provider_chain
        == snapshot.provider_chain);
    assert(snapshot.fallbacks[0].stage == "availability");
    assert(snapshot.fallbacks[0].reason == "TensorRT is unavailable");
    assert(snapshot.fallbacks[1].provider == "CUDAExecutionProvider");
    assert(snapshot.fallbacks[1].resolved_provider_chain
        == snapshot.provider_chain);
    assert(snapshot.fallbacks[1].stage == "session");
    assert(snapshot.fallbacks[1].reason
        == "CUDA session creation failed");
}

void test_service_projects_cpu_runtime_defaults()
{
    ssv::infer::BackendInfo backend;
    auto &onnx = std::get<ssv::infer::OnnxRuntimeBackendInfo>(
        backend.runtime);
    onnx.active_provider_chain = {ssv::SsvProvider::Cpu};

    auto service = ssv::infer::ssv_inference_service_create_with_backend(
        make_inference_config(),
        std::make_unique<FakeBackend>(std::move(backend)));
    const auto snapshot =
        ssv::infer::ssv_inference_service_runtime_snapshot(service.get());

    assert(snapshot.provider_chain == "CPUExecutionProvider");
    assert(snapshot.provider_device == "unknown");
    assert(snapshot.precision == "unknown");
    assert(snapshot.model_hash == "unknown");
    assert(snapshot.input_contract == "rgba_u8_nhwc_v1");
    assert(snapshot.cache_status == "disabled");
    assert(snapshot.fallbacks.empty());
}

void test_service_projects_tensorrt_runtime_snapshot()
{
    ssv::infer::BackendInfo backend;
    backend.runtime = ssv::infer::TensorRtEngineBackendInfo {
        .engine_hash = std::string(64, 'b'),
        .wrapper_hash = std::string(64, 'c'),
        .tensorrt_version = "11.1.0.106",
        .cuda_runtime_version = 13020,
        .compute_capability_major = 8,
        .compute_capability_minor = 9,
    };
    backend.active_device_id = 2;
    backend.resolved_precision = ssv::SsvPrecision::Fp32;

    auto service = ssv::infer::ssv_inference_service_create_with_backend(
        make_inference_config(),
        std::make_unique<FakeBackend>(std::move(backend)));
    const auto snapshot =
        ssv::infer::ssv_inference_service_runtime_snapshot(service.get());

    assert(snapshot.provider_chain == "TensorRTEngine");
    assert(snapshot.provider_device
        == "device:2/compute_capability:8.9");
    assert(snapshot.precision == "fp32");
    assert(snapshot.model_hash == std::string(64, 'c'));
    assert(snapshot.input_contract == "rgba_u8_nhwc_v1");
    assert(snapshot.cache_status == "disabled");
    assert(snapshot.fallbacks.empty());
}

void test_runtime_snapshot_is_stable_and_owning()
{
    ssv::infer::BackendInfo backend;
    auto &onnx = std::get<ssv::infer::OnnxRuntimeBackendInfo>(
        backend.runtime);
    onnx.active_provider_chain = {ssv::SsvProvider::Cpu};
    onnx.fallbacks = {{
        .provider = ssv::SsvProvider::Cuda,
        .stage = ssv::infer::SsvProviderFailureStage::Append,
        .reason = "CUDA provider append failed",
    }};
    onnx.model_hash = std::string(64, 'e');
    backend.resolved_precision = ssv::SsvPrecision::Auto;

    ssv::infer::SsvInferenceRuntimeSnapshot snapshot;
    {
        auto service =
            ssv::infer::ssv_inference_service_create_with_backend(
                make_inference_config(),
                std::make_unique<FakeBackend>(std::move(backend)));
        snapshot = ssv::infer::ssv_inference_service_runtime_snapshot(
            service.get());

        ssv::infer::ssv_inference_service_stop(service.get());
        const auto after_stop =
            ssv::infer::ssv_inference_service_runtime_snapshot(
                service.get());
        assert(after_stop.provider_chain == snapshot.provider_chain);
        assert(after_stop.provider_device == snapshot.provider_device);
        assert(after_stop.precision == snapshot.precision);
        assert(after_stop.model_hash == snapshot.model_hash);
        assert(after_stop.input_contract == snapshot.input_contract);
        assert(after_stop.cache_status == snapshot.cache_status);
        assert(after_stop.fallbacks.size() == snapshot.fallbacks.size());
        assert(after_stop.fallbacks[0].provider
            == snapshot.fallbacks[0].provider);
        assert(after_stop.fallbacks[0].resolved_provider_chain
            == snapshot.fallbacks[0].resolved_provider_chain);
        assert(after_stop.fallbacks[0].stage
            == snapshot.fallbacks[0].stage);
        assert(after_stop.fallbacks[0].reason
            == snapshot.fallbacks[0].reason);
    }

    assert(snapshot.provider_chain == "CPUExecutionProvider");
    assert(snapshot.provider_device == "unknown");
    assert(snapshot.precision == "auto");
    assert(snapshot.model_hash == std::string(64, 'e'));
    assert(snapshot.input_contract == "rgba_u8_nhwc_v1");
    assert(snapshot.cache_status == "disabled");
    assert(snapshot.fallbacks.size() == 1);
    assert(snapshot.fallbacks[0].reason
        == "CUDA provider append failed");
}

void test_runtime_snapshot_preserves_fallback_order_and_stage_names()
{
    const std::array stages = {
        ssv::infer::SsvProviderFailureStage::Configuration,
        ssv::infer::SsvProviderFailureStage::Availability,
        ssv::infer::SsvProviderFailureStage::Append,
        ssv::infer::SsvProviderFailureStage::Session,
        ssv::infer::SsvProviderFailureStage::Cache,
    };
    const std::array<std::string_view, 5> expected_stage_names = {
        "configuration",
        "availability",
        "append",
        "session",
        "cache",
    };

    ssv::infer::BackendInfo backend;
    auto &onnx = std::get<ssv::infer::OnnxRuntimeBackendInfo>(
        backend.runtime);
    onnx.active_provider_chain = {ssv::SsvProvider::Cpu};
    for (std::size_t index = 0; index < stages.size(); ++index) {
        onnx.fallbacks.push_back({
            .provider = ssv::SsvProvider::Cuda,
            .stage = stages[index],
            .reason = "failure-" + std::to_string(index),
        });
    }

    auto service = ssv::infer::ssv_inference_service_create_with_backend(
        make_inference_config(),
        std::make_unique<FakeBackend>(std::move(backend)));
    const auto snapshot =
        ssv::infer::ssv_inference_service_runtime_snapshot(service.get());

    assert(snapshot.fallbacks.size() == stages.size());
    for (std::size_t index = 0; index < stages.size(); ++index) {
        assert(snapshot.fallbacks[index].provider
            == "CUDAExecutionProvider");
        assert(snapshot.fallbacks[index].resolved_provider_chain
            == "CPUExecutionProvider");
        assert(snapshot.fallbacks[index].stage
            == expected_stage_names[index]);
        assert(snapshot.fallbacks[index].reason
            == "failure-" + std::to_string(index));
    }
}

void test_runtime_snapshot_rejects_invalid_services()
{
    auto *invalid_service = SSV_INFERENCE_SERVICE(
        g_object_new(SSV_TYPE_INFERENCE_SERVICE, nullptr));
    assert(invalid_service != nullptr);

    for (auto *service : {
             static_cast<SsvInferenceService *>(nullptr),
             invalid_service,
         }) {
        try {
            static_cast<void>(
                ssv::infer::ssv_inference_service_runtime_snapshot(
                    service));
            assert(false && "invalid inference service was accepted");
        } catch (const std::invalid_argument &error) {
            assert(std::string_view(error.what())
                == "inference service must be created through its public "
                   "factory");
        }
    }

    g_object_unref(invalid_service);
}

void test_service_reports_raw_model_as_contract_failure()
{
    try {
        static_cast<void>(
            ssv::infer::ssv_inference_service_create_with_backend(
                make_inference_config(),
                std::make_unique<RawModelBackend>()));
        assert(false && "service accepted raw model metadata");
    } catch (const ssv::infer::SsvInferenceServiceError &error) {
        assert(error.stage() == "inference.model_contract");
        assert(std::string(error.what()).find("ssv.wrapper.contract")
            != std::string::npos);
    }
}

void test_service_rejects_empty_label_map_before_backend_load()
{
    auto config = make_inference_config();
    config.model.label_map.clear();
    try {
        static_cast<void>(
            ssv::infer::ssv_inference_service_create_with_backend(
                config, std::make_unique<FakeBackend>()));
        assert(false && "service accepted an empty label map");
    } catch (const ssv::infer::SsvInferenceServiceError &error) {
        assert(error.stage() == "inference.start");
        assert(std::string(error.what()).find("inference.model.label_map")
            != std::string::npos);
    }
}

void expect_tensorrt_manifest_rejected(
    const std::filesystem::path &manifest_path,
    std::string_view expected_message)
{
    auto config = make_inference_config();
    config.model.manifest = manifest_path.string();
    config.runtime = ssv::SsvTensorRtEngineConfig {};
    try {
        static_cast<void>(ssv::infer::make_inference_config(config));
        assert(false && "invalid TensorRT manifest path was accepted");
    } catch (const std::invalid_argument &error) {
        assert(std::string_view(error.what()).find(expected_message)
            != std::string_view::npos);
    }
}

void test_tensorrt_manifest_must_be_a_regular_file()
{
    TemporaryDirectory temporary;
    const auto missing_manifest = temporary.path() / "missing.json";

    expect_tensorrt_manifest_rejected(
        missing_manifest, "manifest file not found");
    expect_tensorrt_manifest_rejected(
        temporary.path(), "manifest path is not a regular file");
}

void test_service_owns_the_model_sized_analysis_frame_pool()
{
    auto service = ssv::infer::ssv_inference_service_create_with_backend(
        make_inference_config(), std::make_unique<FakeBackend>());
    GstVideoInfo info;
    gst_video_info_init(&info);
    assert(gst_video_info_set_format(&info, GST_VIDEO_FORMAT_RGBA, 3, 2));
    GstBuffer *buffer = gst_buffer_new_allocate(nullptr, 24, nullptr);
    assert(buffer != nullptr);

    auto frame = ssv::infer::ssv_inference_service_create_analysis_frame(
        service.get(),
        buffer,
        info,
        {3, 2, 3, 2, 1.0F, 0, 0, 0, 0},
        {GST_SECOND, GST_SECOND / 15, 2});
    gst_buffer_unref(buffer);

    assert(frame->view().width == 3);
    assert(frame->view().height == 2);
    assert(frame->timing().generation == 2);
    auto stats = ssv::infer::ssv_inference_service_stats(service.get());
    assert(stats.analysis_frames.map_count == 1);
    assert(stats.analysis_frames.active_maps == 1);

    frame.reset();
    stats = ssv::infer::ssv_inference_service_stats(service.get());
    assert(stats.analysis_frames.active_maps == 0);
    assert(stats.analysis_frames.outstanding_staging_leases == 0);
}

void test_service_exposes_model_contract_and_source_letterbox_transform()
{
    auto service = ssv::infer::ssv_inference_service_create_with_backend(
        make_inference_config(), std::make_unique<FakeBackend>());

    const auto contract =
        ssv::infer::ssv_inference_service_model_contract(service.get());
    assert(contract.width == 3);
    assert(contract.height == 2);

    try {
        static_cast<void>(
            ssv::infer::ssv_inference_service_preprocess_transform(
                service.get(), "camera-01"));
        assert(false && "unknown source geometry was accepted");
    } catch (const std::logic_error &) {
    }

    ssv::infer::ssv_inference_service_update_source_geometry(
        service.get(), "camera-01", 4, 4);
    auto transform =
        ssv::infer::ssv_inference_service_preprocess_transform(
            service.get(), "camera-01");
    assert(transform.source_width == 4);
    assert(transform.source_height == 4);
    assert(transform.model_width == 3);
    assert(transform.model_height == 2);
    assert(transform.scale == 0.5F);
    assert(transform.pad_left == 0);
    assert(transform.pad_right == 1);
    assert(transform.pad_top == 0);
    assert(transform.pad_bottom == 0);

    ssv::infer::ssv_inference_service_update_source_geometry(
        service.get(), "camera-01", 6, 2);
    transform = ssv::infer::ssv_inference_service_preprocess_transform(
        service.get(), "camera-01");
    assert(transform.scale == 0.5F);
    assert(transform.pad_top == 0);
    assert(transform.pad_bottom == 1);
}

void test_completed_detection_shares_the_analysis_frame()
{
    auto service = ssv::infer::ssv_inference_service_create_with_backend(
        make_inference_config(), std::make_unique<FakeBackend>());
    GstVideoInfo info;
    gst_video_info_init(&info);
    assert(gst_video_info_set_format(&info, GST_VIDEO_FORMAT_RGBA, 3, 2));
    GstBuffer *buffer = gst_buffer_new_allocate(nullptr, 24, nullptr);
    assert(buffer != nullptr);
    auto frame = ssv::infer::ssv_inference_service_create_analysis_frame(
        service.get(),
        buffer,
        info,
        {6, 4, 3, 2, 0.5F, 0, 0, 0, 0},
        {5 * GST_SECOND, GST_SECOND / 15, 7});
    gst_buffer_unref(buffer);

    ssv::infer::SsvInferenceRequest request;
    request.frame_id = 42;
    request.source_id = "camera-01";
    request.analysis_frame = frame;
    auto result = ssv::infer::ssv_inference_service_submit(
        service.get(), request);

    assert(result.status
        == ssv::infer::SsvInferenceSubmissionStatus::Completed);
    assert(result.detections.frame_id == 42);
    assert(result.detections.timing == frame->timing());
    assert(result.detections.analysis_frame == frame);
    assert(result.detections.detections.size() == 1);
    assert(ssv::infer::ssv_inference_service_stats(service.get())
        .analysis_frames.active_maps == 1);

    request.analysis_frame.reset();
    frame.reset();
    assert(ssv::infer::ssv_inference_service_stats(service.get())
        .analysis_frames.active_maps == 1);
    result.detections.analysis_frame.reset();
    assert(wait_for_active_maps(service.get(), 0));
}

void test_service_preserves_timing_and_reuses_fixed_buffers()
{
    const auto config = make_inference_config();

    auto allocator = std::make_shared<CountingAllocator>();
    auto service = ssv::infer::ssv_inference_service_create_with_backend(
        config, std::make_unique<FakeBackend>(), allocator);
    assert(service != nullptr);
    assert(ssv::infer::ssv_inference_service_is_running(service.get()));
    assert(allocator->allocation_count == 1);

    auto request = make_request(service.get(), 42);
    const auto expected_timing = request.analysis_frame->timing();

    for (int iteration = 0; iteration < 2; ++iteration) {
        const auto result = ssv::infer::ssv_inference_service_submit(
            service.get(), request);
        assert(result.status
            == ssv::infer::SsvInferenceSubmissionStatus::Completed);
        assert(result.detections.frame_id == request.frame_id);
        assert(result.detections.source_id == request.source_id);
        assert(result.detections.timing == expected_timing);
        assert(result.detections.detections.size() == 1);
    }

    assert(allocator->allocation_count == 1);
    const auto stats =
        ssv::infer::ssv_inference_service_stats(service.get());
    assert(stats.submitted == 2);
    assert(stats.completed == 2);
    assert(stats.in_flight == 0);
    assert(stats.pending == 0);
    request.analysis_frame.reset();
    assert(ssv::infer::ssv_inference_service_stats(service.get())
        .analysis_frames.active_maps == 0);

    ssv::infer::ssv_inference_service_stop(service.get());
    assert(!ssv::infer::ssv_inference_service_is_running(service.get()));
}

void test_service_exposes_and_resets_inference_stats_windows()
{
    auto service = ssv::infer::ssv_inference_service_create_with_backend(
        make_inference_config(), std::make_unique<FakeBackend>());
    for (std::uint64_t frame_id = 1; frame_id <= 2; ++frame_id) {
        auto request = make_request(service.get(), frame_id);
        auto result = ssv::infer::ssv_inference_service_submit(
            service.get(), std::move(request));
        assert(result.status
            == ssv::infer::SsvInferenceSubmissionStatus::Completed);
    }

    const auto window =
        ssv::infer::ssv_inference_service_take_stats_window(service.get());
    assert(window.received == 2);
    assert(window.dropped == 0);
    assert(window.completed == 2);
    assert(window.completed_fps > 0.0);
    assert(window.device.p50_us >= 1'000);
    assert(window.device.p95_us >= window.device.p50_us);
    assert(window.output_copy.p50_us == 0);
    assert(window.total.p50_us >= window.device.p50_us);

    const auto empty =
        ssv::infer::ssv_inference_service_take_stats_window(service.get());
    assert(empty.received == 0);
    assert(empty.dropped == 0);
    assert(empty.completed == 0);
}

void test_total_latency_includes_queue_wait()
{
    auto backend = std::make_unique<ControlledBackend>();
    auto *backend_control = backend.get();
    auto service = ssv::infer::ssv_inference_service_create_with_backend(
        make_inference_config(), std::move(backend));

    auto first_request = make_request(service.get(), 1, "camera-01", true);
    auto second_request = make_request(service.get(), 2, "camera-01", true);
    auto first = std::async(std::launch::async,
        [&, request = std::move(first_request)]() mutable {
            return ssv::infer::ssv_inference_service_submit(
                service.get(), std::move(request));
        });
    assert(backend_control->wait_until_entered(1));
    auto second = std::async(std::launch::async,
        [&, request = std::move(second_request)]() mutable {
            return ssv::infer::ssv_inference_service_submit(
                service.get(), std::move(request));
        });
    assert(wait_for_pending(service.get(), 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    backend_control->allow_one_completion();
    auto first_result = first.get();
    assert(first_result.status
        == ssv::infer::SsvInferenceSubmissionStatus::Completed);
    assert(backend_control->wait_until_entered(2));
    static_cast<void>(
        ssv::infer::ssv_inference_service_take_stats_window(service.get()));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    backend_control->allow_one_completion();
    auto second_result = second.get();
    assert(second_result.status
        == ssv::infer::SsvInferenceSubmissionStatus::Completed);
    const auto window =
        ssv::infer::ssv_inference_service_take_stats_window(service.get());
    assert(window.completed == 1);
    assert(window.queue.p50_us >= 10'000);
    assert(window.device.p50_us >= 10'000);
    assert(window.total.p50_us
        >= window.queue.p50_us + window.device.p50_us);
}

void test_service_replaces_only_the_latest_pending_request()
{
    auto backend = std::make_unique<ControlledBackend>();
    auto *backend_control = backend.get();
    auto service = ssv::infer::ssv_inference_service_create_with_backend(
        make_inference_config(), std::move(backend));

    auto first_request = make_request(service.get(), 1, "camera-01", true);
    auto second_request = make_request(service.get(), 2, "camera-01", true);
    auto third_request = make_request(service.get(), 3, "camera-01", true);

    auto first = std::async(std::launch::async,
        [&, request = std::move(first_request)]() mutable {
            return ssv::infer::ssv_inference_service_submit(
                service.get(), std::move(request));
        });
    assert(backend_control->wait_until_entered(1));

    auto second = std::async(std::launch::async,
        [&, request = std::move(second_request)]() mutable {
            return ssv::infer::ssv_inference_service_submit(
                service.get(), std::move(request));
        });
    assert(wait_for_pending(service.get(), 1));

    auto third = std::async(std::launch::async,
        [&, request = std::move(third_request)]() mutable {
            return ssv::infer::ssv_inference_service_submit(
                service.get(), std::move(request));
        });
    assert(second.wait_for(std::chrono::seconds(2))
        == std::future_status::ready);
    const auto second_result = second.get();
    assert(second_result.status
        == ssv::infer::SsvInferenceSubmissionStatus::Replaced);
    assert(wait_for_staging_leases(service.get(), 2));

    backend_control->allow_one_completion();
    assert(first.get().status
        == ssv::infer::SsvInferenceSubmissionStatus::Completed);
    assert(wait_for_staging_leases(service.get(), 1));
    assert(backend_control->wait_until_entered(2));
    backend_control->allow_one_completion();
    auto third_result = third.get();
    assert(third_result.status
        == ssv::infer::SsvInferenceSubmissionStatus::Completed);
    assert(third_result.detections.frame_id == 3);

    const auto stats =
        ssv::infer::ssv_inference_service_stats(service.get());
    assert(stats.submitted == 3);
    assert(stats.completed == 2);
    assert(stats.replaced == 1);
    assert(stats.max_in_flight == 1);
    assert(stats.max_pending == 1);
    assert(backend_control->max_active_count() == 1);
    assert(stats.analysis_frames.active_maps == 0);
    assert(stats.analysis_frames.outstanding_staging_leases == 1);
    third_result.detections.analysis_frame.reset();
    assert(ssv::infer::ssv_inference_service_stats(service.get())
        .analysis_frames.outstanding_staging_leases == 0);
}

void test_cancel_and_stop_release_submitters()
{
    auto backend = std::make_unique<ControlledBackend>();
    auto *backend_control = backend.get();
    auto service = ssv::infer::ssv_inference_service_create_with_backend(
        make_inference_config(), std::move(backend));

    auto first_request = make_request(
        service.get(), 1, "cancelled-source", true);
    auto second_request = make_request(
        service.get(), 2, "cancelled-source", true);

    auto first = std::async(std::launch::async,
        [&, request = std::move(first_request)]() mutable {
            return ssv::infer::ssv_inference_service_submit(
                service.get(), std::move(request));
        });
    assert(backend_control->wait_until_entered(1));
    auto second = std::async(std::launch::async,
        [&, request = std::move(second_request)]() mutable {
            return ssv::infer::ssv_inference_service_submit(
                service.get(), std::move(request));
        });
    assert(wait_for_pending(service.get(), 1));

    ssv::infer::ssv_inference_service_cancel(
        service.get(), "cancelled-source");
    assert(second.wait_for(std::chrono::seconds(2))
        == std::future_status::ready);
    assert(second.get().status
        == ssv::infer::SsvInferenceSubmissionStatus::Cancelled);
    assert(first.wait_for(std::chrono::seconds(2))
        == std::future_status::ready);
    assert(first.get().status
        == ssv::infer::SsvInferenceSubmissionStatus::Cancelled);
    assert(ssv::infer::ssv_inference_service_is_running(service.get()));
    assert(ssv::infer::ssv_inference_service_stats(service.get())
        .analysis_frames.outstanding_staging_leases == 0);

    auto third_request = make_request(service.get(), 3, "camera-01", true);
    auto fourth_request = make_request(service.get(), 4, "camera-01", true);
    auto third = std::async(std::launch::async,
        [&, request = std::move(third_request)]() mutable {
            return ssv::infer::ssv_inference_service_submit(
                service.get(), std::move(request));
        });
    assert(backend_control->wait_until_entered(2));
    auto fourth = std::async(std::launch::async,
        [&, request = std::move(fourth_request)]() mutable {
            return ssv::infer::ssv_inference_service_submit(
                service.get(), std::move(request));
        });
    assert(wait_for_pending(service.get(), 1));

    auto stopping = std::async(std::launch::async, [&] {
        ssv::infer::ssv_inference_service_stop(service.get());
    });
    assert(fourth.wait_for(std::chrono::seconds(2))
        == std::future_status::ready);
    assert(fourth.get().status
        == ssv::infer::SsvInferenceSubmissionStatus::Cancelled);
    assert(third.wait_for(std::chrono::seconds(2))
        == std::future_status::ready);
    assert(third.get().status
        == ssv::infer::SsvInferenceSubmissionStatus::Cancelled);
    assert(stopping.wait_for(std::chrono::seconds(2))
        == std::future_status::ready);
    stopping.get();
    assert(!ssv::infer::ssv_inference_service_is_running(service.get()));

    ssv::infer::SsvInferenceRequest stopped_request;
    stopped_request.frame_id = 5;
    stopped_request.source_id = "camera-01";
    const auto stopped_result =
        ssv::infer::ssv_inference_service_submit(
            service.get(), std::move(stopped_request));
    assert(stopped_result.status
        == ssv::infer::SsvInferenceSubmissionStatus::Cancelled);
    const auto stats =
        ssv::infer::ssv_inference_service_stats(service.get());
    assert(stats.submitted == 4);
    assert(stats.cancelled == 4);
    assert(stats.in_flight == 0);
    assert(stats.pending == 0);
    assert(stats.analysis_frames.active_maps == 0);
    assert(stats.analysis_frames.outstanding_staging_leases == 0);
}

void test_backend_failure_does_not_stop_the_worker()
{
    auto backend = std::make_unique<ControlledBackend>();
    auto *backend_control = backend.get();
    backend_control->fail_on_call(1);
    auto service = ssv::infer::ssv_inference_service_create_with_backend(
        make_inference_config(), std::move(backend));

    auto first_request = make_request(service.get(), 1, "camera-01", true);
    backend_control->allow_one_completion();
    const auto first_result =
        ssv::infer::ssv_inference_service_submit(
            service.get(), std::move(first_request));
    assert(first_result.status
        == ssv::infer::SsvInferenceSubmissionStatus::Failed);
    assert(first_result.error == "controlled inference failure");
    assert(ssv::infer::ssv_inference_service_is_running(service.get()));
    assert(ssv::infer::ssv_inference_service_stats(service.get())
        .analysis_frames.outstanding_staging_leases == 0);

    auto second_request = make_request(service.get(), 2, "camera-01", true);
    backend_control->allow_one_completion();
    auto second_result =
        ssv::infer::ssv_inference_service_submit(
            service.get(), std::move(second_request));
    assert(second_result.status
        == ssv::infer::SsvInferenceSubmissionStatus::Completed);
    assert(second_result.detections.frame_id == 2);

    const auto stats =
        ssv::infer::ssv_inference_service_stats(service.get());
    assert(stats.submitted == 2);
    assert(stats.failed == 1);
    assert(stats.completed == 1);
    assert(stats.max_in_flight == 1);
    assert(stats.max_pending == 1);
    assert(stats.analysis_frames.outstanding_staging_leases == 1);
    second_result.detections.analysis_frame.reset();
    assert(ssv::infer::ssv_inference_service_stats(service.get())
        .analysis_frames.outstanding_staging_leases == 0);
}

} // namespace

int main(int argc, char **argv)
{
    gst_init(&argc, &argv);
    test_inference_stats_summarizes_a_fixed_window();
    test_model_contract_accepts_only_wrapper_input();
    test_service_projects_onnx_runtime_snapshot();
    test_service_projects_cpu_runtime_defaults();
    test_service_projects_tensorrt_runtime_snapshot();
    test_runtime_snapshot_is_stable_and_owning();
    test_runtime_snapshot_preserves_fallback_order_and_stage_names();
    test_runtime_snapshot_rejects_invalid_services();
    test_service_reports_raw_model_as_contract_failure();
    test_service_rejects_empty_label_map_before_backend_load();
    test_tensorrt_manifest_must_be_a_regular_file();
    test_service_owns_the_model_sized_analysis_frame_pool();
    test_service_exposes_model_contract_and_source_letterbox_transform();
    test_completed_detection_shares_the_analysis_frame();
    test_service_preserves_timing_and_reuses_fixed_buffers();
    test_service_exposes_and_resets_inference_stats_windows();
    test_total_latency_includes_queue_wait();
    test_service_replaces_only_the_latest_pending_request();
    test_cancel_and_stop_release_submitters();
    test_backend_failure_does_not_stop_the_worker();
    return 0;
}
