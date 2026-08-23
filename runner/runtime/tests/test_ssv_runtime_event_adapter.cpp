#include "runtime/ssv_runtime_event_adapter.hpp"

#include <cassert>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace {

void test_disabled_runtime_is_snapshotted_without_runtime_handles()
{
    ssv::SsvConfig config;
    ssv::SsvSourceConfig source;
    source.id = "camera-01";
    source.uri = "rtsp://127.0.0.1/test";
    config.sources.push_back(std::move(source));
    config.display.enabled = false;
    config.inference.enabled = false;

    ssv::SsvPipelinePlan plan;
    plan.source_id = "camera-01";
    plan.decode = {
        .backend = ssv::SsvDecodeBackend::Software,
        .device = {},
        .decoder_factory = "avdec_h264",
        .va_postproc_factory = {},
        .software_fallback_allowed = false,
    };
    plan.expected_caps.decode_output = {
        ssv::SsvPixelFormat::Nv12,
        ssv::SsvMemoryKind::SystemMemory,
    };

    const auto event = ssv::ssv_runtime_resolved_event(
        {.source_id = "camera-01", .run_attempt_id = 1},
        config,
        plan,
        std::nullopt);
    const auto &payload = std::get<ssv::SsvRuntimeResolvedEvent>(
        event.payload);

    assert(event.context.source_id == "camera-01");
    assert(event.context.run_attempt_id == 1);
    assert(payload.decoder == "avdec_h264");
    assert(payload.va_device == "not-applicable");
    assert(payload.va_driver == "not-applicable");
    assert(payload.decode_memory == "SystemMemory");
    assert(payload.vpp == "disabled");
    assert(payload.display_backend == "disabled");
    assert(payload.egl_renderer == "not-applicable");
    assert(payload.provider_chain == "disabled");
    assert(payload.provider_device == "not-applicable");
    assert(payload.precision == "not-applicable");
    assert(payload.model_hash == "not-applicable");
    assert(payload.input_contract == "not-applicable");
    assert(payload.cache_status == "disabled");
}

void test_tensorrt_runtime_uses_source_model_identity_and_device_capability()
{
    ssv::SsvConfig config;
    ssv::SsvSourceConfig source;
    source.id = "camera-01";
    source.uri = "rtsp://127.0.0.1/test";
    config.sources.push_back(std::move(source));
    config.display.enabled = false;
    config.inference.enabled = true;

    ssv::SsvPipelinePlan plan;
    plan.source_id = "camera-01";
    plan.decode = {
        .backend = ssv::SsvDecodeBackend::Software,
        .device = {},
        .decoder_factory = "avdec_h264",
        .va_postproc_factory = {},
        .software_fallback_allowed = false,
    };
    plan.expected_caps.decode_output = {
        ssv::SsvPixelFormat::Nv12,
        ssv::SsvMemoryKind::SystemMemory,
    };
    const std::optional<ssv::infer::SsvInferenceRuntimeSnapshot> snapshot {
        {
            .provider_chain = "TensorRTEngine",
            .provider_device = "device:2/compute_capability:8.9",
            .precision = "fp16",
            .model_hash = std::string(64, 'c'),
            .input_contract = "float32[1,3,640,640]:NCHW",
            .cache_status = "disabled",
            .fallbacks = {},
        },
    };

    const auto event = ssv::ssv_runtime_resolved_event(
        {.source_id = "camera-01", .run_attempt_id = 2},
        config,
        plan,
        snapshot);
    const auto &payload = std::get<ssv::SsvRuntimeResolvedEvent>(
        event.payload);

    assert(payload.provider_chain == "TensorRTEngine");
    assert(payload.provider_device == "device:2/compute_capability:8.9");
    assert(payload.precision == "fp16");
    assert(payload.model_hash == std::string(64, 'c'));
    assert(payload.model_hash != std::string(64, 'b'));
    assert(payload.input_contract == "float32[1,3,640,640]:NCHW");
    assert(payload.cache_status == "disabled");
}

void test_runtime_snapshot_presence_must_match_inference_config()
{
    ssv::SsvConfig config;
    ssv::SsvSourceConfig source;
    source.id = "camera-01";
    source.uri = "rtsp://127.0.0.1/test";
    config.sources.push_back(std::move(source));
    config.display.enabled = false;

    ssv::SsvPipelinePlan plan;
    plan.source_id = "camera-01";
    plan.decode = {
        .backend = ssv::SsvDecodeBackend::Software,
        .device = {},
        .decoder_factory = "avdec_h264",
        .va_postproc_factory = {},
        .software_fallback_allowed = false,
    };
    plan.expected_caps.decode_output = {
        ssv::SsvPixelFormat::Nv12,
        ssv::SsvMemoryKind::SystemMemory,
    };

    const auto expect_rejected = [&](bool inference_enabled,
                                     const auto &snapshot) {
        config.inference.enabled = inference_enabled;
        try {
            static_cast<void>(ssv::ssv_runtime_resolved_event(
                {.source_id = "camera-01", .run_attempt_id = 2},
                config,
                plan,
                snapshot));
            assert(false && "inference/snapshot mismatch was accepted");
        } catch (const std::invalid_argument &error) {
            assert(std::string_view(error.what()).find(
                       "runtime snapshot presence")
                != std::string_view::npos);
        }
    };

    const std::optional<ssv::infer::SsvInferenceRuntimeSnapshot>
        no_snapshot;
    expect_rejected(true, no_snapshot);
    const std::optional<ssv::infer::SsvInferenceRuntimeSnapshot>
        unexpected_snapshot {std::in_place};
    expect_rejected(false, unexpected_snapshot);
}

void test_provider_fallbacks_copy_snapshot_values_in_order()
{
    const ssv::infer::SsvInferenceRuntimeSnapshot snapshot {
        .provider_chain = "CUDAExecutionProvider,CPUExecutionProvider",
        .provider_device = "device:0",
        .precision = "fp16",
        .model_hash = std::string(64, 'a'),
        .input_contract = "float32[1,3,640,640]:NCHW",
        .cache_status = "hit",
        .fallbacks = {
            {
                .provider = "TensorrtExecutionProvider",
                .resolved_provider_chain =
                    "CUDAExecutionProvider,CPUExecutionProvider",
                .stage = "availability",
                .reason = "TensorRT EP unavailable",
            },
            {
                .provider = "CUDAExecutionProvider",
                .resolved_provider_chain = "CPUExecutionProvider",
                .stage = "session",
                .reason = "CUDA session creation failed",
            },
        },
    };

    const auto events = ssv::ssv_provider_fallback_events(
        {.source_id = "camera-01", .run_attempt_id = 2},
        snapshot);

    assert(events.size() == 2);
    assert(events.front().context.source_id == "camera-01");
    assert(events.front().context.run_attempt_id == 2);
    const auto &payload = std::get<ssv::SsvAccelerationFallbackEvent>(
        events.front().payload);
    assert(payload.from == "TensorrtExecutionProvider");
    assert(payload.to == "CUDAExecutionProvider,CPUExecutionProvider");
    assert(payload.stage == "inference.provider.availability");
    assert(payload.reason == "TensorRT EP unavailable");
    const auto &second_payload =
        std::get<ssv::SsvAccelerationFallbackEvent>(events[1].payload);
    assert(second_payload.from == "CUDAExecutionProvider");
    assert(second_payload.to == "CPUExecutionProvider");
    assert(second_payload.stage == "inference.provider.session");
    assert(second_payload.reason == "CUDA session creation failed");
}

void test_buffer_contract_failure_copies_all_contract_fields()
{
    const ssv::SsvPipelineContractViolation violation {
        ssv::SsvPipelineBoundary::AnalysisHost,
        "video/x-raw(memory:SystemMemory),format=RGBA,width=640,height=640",
        "not-required",
        "SystemMemory",
        "video/x-raw(memory:VAMemory),format=RGBA,width=640,height=640",
        "VAMemory",
        "VAMemory",
        "buffer memory does not match",
    };

    const auto event = ssv::ssv_buffer_contract_failed_event(
        {.source_id = "camera-01", .run_attempt_id = 2},
        "analysis.host",
        violation);
    const auto &payload = std::get<ssv::SsvBufferContractFailedEvent>(
        event.payload);

    assert(payload.stage == "analysis.host");
    assert(payload.expected_caps == violation.expected_caps);
    assert(payload.expected_allocator == violation.expected_allocator);
    assert(payload.expected_memory == violation.expected_memory);
    assert(payload.actual_caps == violation.actual_caps);
    assert(payload.actual_allocator == violation.actual_allocator);
    assert(payload.actual_memory == violation.actual_memory);
    assert(payload.reason == violation.message);
}

void test_inference_stats_converts_microseconds_to_owning_durations()
{
    ssv::infer::SsvInferenceStatsWindow stats;
    stats.received = 10;
    stats.dropped = 2;
    stats.completed = 8;
    stats.completed_fps = 1.6;
    stats.longest_result_gap_us = 750'000;
    stats.queue = {10, 20};
    stats.backend_execution = {100, 200};
    stats.backend_d2h = {};
    stats.postprocess = {30, 60};
    stats.total = {140, 280};

    const auto event = ssv::ssv_inference_stats_event(
        {.source_id = "camera-01", .run_attempt_id = 2},
        stats);
    const auto &payload = std::get<ssv::SsvInferenceStatsEvent>(
        event.payload);

    assert(payload.received == 10);
    assert(payload.dropped == 2);
    assert(payload.completed == 8);
    assert(payload.completed_fps == 1.6);
    assert(payload.longest_result_gap == std::chrono::microseconds {750'000});
    assert(payload.queue.p50 && *payload.queue.p50 == std::chrono::microseconds {10});
    assert(payload.queue.p95 && *payload.queue.p95 == std::chrono::microseconds {20});
    assert(payload.backend_execution.p50
        && *payload.backend_execution.p50 == std::chrono::microseconds {100});
    assert(payload.backend_execution.p95
        && *payload.backend_execution.p95 == std::chrono::microseconds {200});
    assert(!payload.backend_d2h.p50);
    assert(!payload.backend_d2h.p95);
    assert(payload.postprocess.p50
        && *payload.postprocess.p50 == std::chrono::microseconds {30});
    assert(payload.postprocess.p95
        && *payload.postprocess.p95 == std::chrono::microseconds {60});
    assert(payload.total.p50
        && *payload.total.p50 == std::chrono::microseconds {140});
    assert(payload.total.p95
        && *payload.total.p95 == std::chrono::microseconds {280});
}

} // namespace

int main()
{
    test_disabled_runtime_is_snapshotted_without_runtime_handles();
    test_tensorrt_runtime_uses_source_model_identity_and_device_capability();
    test_runtime_snapshot_presence_must_match_inference_config();
    test_provider_fallbacks_copy_snapshot_values_in_order();
    test_buffer_contract_failure_copies_all_contract_fields();
    test_inference_stats_converts_microseconds_to_owning_durations();
    return 0;
}
