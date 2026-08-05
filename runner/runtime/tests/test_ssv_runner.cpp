#include "display/ssv_display_window.hpp"
#include "observability/ssv_event_log.hpp"
#include "pipeline/ssv_pipeline_builder.hpp"
#include "runtime/ssv_run_attempt_factory.hpp"
#include "runtime/ssv_runner.hpp"

#include <cassert>
#include <chrono>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

struct RecordedLogState {
    std::vector<ssv::SsvEncodedLogRecord> records;
};

class RecordingLogSink final : public ssv::SsvLogSink {
public:
    explicit RecordingLogSink(std::shared_ptr<RecordedLogState> state)
        : state_(std::move(state))
    {
    }

    ssv::SsvLogSubmitResult submit(
        ssv::SsvEncodedLogRecord record) noexcept override
    {
        ++stats_.written_records;
        state_->records.push_back(std::move(record));
        return ssv::SsvLogSubmitResult::Accepted;
    }

    ssv::SsvLogIoResult flush(
        std::chrono::steady_clock::time_point) noexcept override
    {
        return {ssv::SsvLogIoStatus::Completed, stats_};
    }

    ssv::SsvLogIoResult close(
        std::chrono::steady_clock::time_point) noexcept override
    {
        return {ssv::SsvLogIoStatus::Completed, stats_};
    }

private:
    std::shared_ptr<RecordedLogState> state_;
    ssv::SsvLogSinkStats stats_;
};

struct AttemptObservation {
    ssv::SsvConfig effective_config;
    ssv::SsvPipelinePlan plan;
    ssv::SsvEventContext context;
};

struct FakeFactoryState {
    std::vector<ssv::SsvRunAttemptResult> scripted_results;
    std::exception_ptr prepare_error;
    std::vector<std::exception_ptr> scripted_creation_errors;
    std::vector<std::vector<ssv::SsvEvent>> scripted_creation_events;
    std::vector<AttemptObservation> attempts;
    std::vector<std::string> lifecycle;
};

class FakeRunAttempt final : public ssv::SsvRunAttempt {
public:
    FakeRunAttempt(
        std::shared_ptr<FakeFactoryState> state,
        std::uint32_t attempt_id,
        ssv::SsvRunAttemptResult result)
        : state_(std::move(state))
        , attempt_id_(attempt_id)
        , result_(std::move(result))
    {
    }

    ~FakeRunAttempt() override
    {
        state_->lifecycle.push_back(
            "destroy:" + std::to_string(attempt_id_));
    }

    ssv::SsvRunAttemptResult run() override
    {
        state_->lifecycle.push_back("run:" + std::to_string(attempt_id_));
        return result_;
    }

private:
    std::shared_ptr<FakeFactoryState> state_;
    std::uint32_t attempt_id_;
    ssv::SsvRunAttemptResult result_;
};

class FakeRunAttemptFactory final : public ssv::SsvRunAttemptFactory {
public:
    FakeRunAttemptFactory(
        std::shared_ptr<FakeFactoryState> state,
        ssv::SsvHardwareCapabilities capabilities)
        : state_(std::move(state))
        , capabilities_(std::move(capabilities))
    {
    }

    ssv::SsvHardwareCapabilities prepare_run(
        const ssv::SsvConfig &) override
    {
        if (state_->prepare_error)
            std::rethrow_exception(state_->prepare_error);
        return capabilities_;
    }

    ssv::SsvRunAttemptCreation create(
        const ssv::SsvConfig &effective_config,
        const ssv::SsvPipelinePlan &plan,
        ssv::SsvEventContext context) override
    {
        const auto index = state_->attempts.size();
        assert(context.run_attempt_id.has_value());
        const auto attempt_id = *context.run_attempt_id;
        state_->attempts.push_back({effective_config, plan, context});
        state_->lifecycle.push_back("create:" + std::to_string(attempt_id));
        if (index < state_->scripted_creation_errors.size()
            && state_->scripted_creation_errors[index]) {
            std::rethrow_exception(
                state_->scripted_creation_errors[index]);
        }
        assert(index < state_->scripted_results.size());
        std::vector<ssv::SsvEvent> events;
        if (index < state_->scripted_creation_events.size()) {
            events = std::move(state_->scripted_creation_events[index]);
            for (auto &event : events)
                event.context = context;
        }
        return {
            .attempt = std::make_unique<FakeRunAttempt>(
                state_, attempt_id, state_->scripted_results[index]),
            .events = std::move(events),
        };
    }

private:
    std::shared_ptr<FakeFactoryState> state_;
    ssv::SsvHardwareCapabilities capabilities_;
};

ssv::SsvConfig make_config()
{
    ssv::SsvConfig config;
    ssv::SsvSourceConfig source;
    source.id = "runner-test";
    source.uri = "rtsp://127.0.0.1/test";
    config.sources.push_back(std::move(source));
    config.display.enabled = false;
    config.inference.enabled = false;
    return config;
}

ssv::SsvHardwareCapabilities make_software_capabilities()
{
    ssv::SsvHardwareCapabilities capabilities;
    capabilities.gstreamer_elements = {"avdec_h264"};
    return capabilities;
}

ssv::SsvHardwareCapabilities make_accelerated_display_capabilities()
{
    ssv::SsvHardwareCapabilities capabilities;
    capabilities.gstreamer_elements = {
        "vah264dec",
        "vapostproc",
        "avdec_h264",
        "gtkglsink",
        "gtksink",
        "glupload",
        "glcolorconvert",
    };
    return capabilities;
}

ssv::SsvHardwareCapabilities make_auto_decode_capabilities()
{
    ssv::SsvHardwareCapabilities capabilities;
    capabilities.gstreamer_elements = {
        "varenderD129h264dec",
        "varenderD129postproc",
        "avdec_h264",
    };
    return capabilities;
}

ssv::SsvHardwareCapabilities make_nvdec_fallback_capabilities()
{
    ssv::SsvHardwareCapabilities capabilities;
    capabilities.gstreamer_elements = {
        "nvh264dec",
        "avdec_h264",
    };
    return capabilities;
}

ssv::SsvHardwareCapabilities make_gtk_system_memory_capabilities()
{
    ssv::SsvHardwareCapabilities capabilities;
    capabilities.gstreamer_elements = {
        "avdec_h264",
        "gtksink",
    };
    return capabilities;
}

void test_successful_run_uses_one_owned_attempt_with_id_one()
{
    auto state = std::make_shared<FakeFactoryState>();
    state->scripted_results.push_back({
        ssv::SsvExitCode::Success,
        ssv::SsvRunAttemptStopReason::EndOfStream,
        true,
        {},
        {},
    });
    auto log_state = std::make_shared<RecordedLogState>();
    auto event_log = ssv::SsvEventLog::create(
        {}, std::make_unique<RecordingLogSink>(log_state));
    auto runner = ssv::ssv_runner_create_with_factory(
        make_config(),
        *event_log,
        std::make_unique<FakeRunAttemptFactory>(
            state, make_software_capabilities()));

    const auto result = runner->run();

    assert(result.exit_code == ssv::SsvExitCode::Success);
    assert(result.stage.empty());
    assert(result.error.empty());
    assert(state->attempts.size() == 1);
    assert(state->attempts.front().context.source_id == "runner-test");
    assert(state->attempts.front().context.run_attempt_id == 1);
    assert(state->lifecycle == std::vector<std::string>({
        "create:1",
        "run:1",
        "destroy:1",
    }));
}

void test_auto_display_failure_retries_after_destroying_failed_attempt()
{
    auto state = std::make_shared<FakeFactoryState>();
    state->scripted_results = {
        {
            ssv::SsvExitCode::DisplayInitializationFailed,
            ssv::SsvRunAttemptStopReason::RuntimeError,
            false,
            "display.initialize",
            "failed to create GL context",
        },
        {
            ssv::SsvExitCode::Success,
            ssv::SsvRunAttemptStopReason::EndOfStream,
            true,
            {},
            {},
        },
    };
    auto config = make_config();
    config.display.enabled = true;
    config.display.backend = ssv::SsvDisplayBackend::Auto;
    auto log_state = std::make_shared<RecordedLogState>();
    auto event_log = ssv::SsvEventLog::create(
        {}, std::make_unique<RecordingLogSink>(log_state));
    auto runner = ssv::ssv_runner_create_with_factory(
        std::move(config),
        *event_log,
        std::make_unique<FakeRunAttemptFactory>(
            state, make_accelerated_display_capabilities()));

    const auto result = runner->run();

    assert(result.exit_code == ssv::SsvExitCode::Success);
    assert(state->attempts.size() == 2);
    assert(state->attempts[0].effective_config.display.backend
        == ssv::SsvDisplayBackend::Auto);
    assert(state->attempts[0].plan.display_backend
        == ssv::SsvResolvedDisplayBackend::GtkGlSink);
    assert(state->attempts[0].context.run_attempt_id == 1);
    assert(state->attempts[1].effective_config.display.backend
        == ssv::SsvDisplayBackend::GtkSink);
    assert(state->attempts[1].plan.display_backend
        == ssv::SsvResolvedDisplayBackend::GtkSink);
    assert(state->attempts[1].context.run_attempt_id == 2);
    assert(state->lifecycle == std::vector<std::string>({
        "create:1",
        "run:1",
        "destroy:1",
        "create:2",
        "run:2",
        "destroy:2",
    }));
    assert(log_state->records.size() == 1);
    const auto &record = log_state->records.front().bytes;
    assert(record.find("event=acceleration_fallback")
        != std::string::npos);
    assert(record.find("run_attempt_id=1") != std::string::npos);
    assert(record.find("from=gtkglsink to=gtksink")
        != std::string::npos);
    assert(record.find("stage=display.initialize")
        != std::string::npos);
}

void test_auto_decode_contract_failure_retries_in_software()
{
    auto state = std::make_shared<FakeFactoryState>();
    state->scripted_results = {
        {
            ssv::SsvExitCode::PipelineContractFailed,
            ssv::SsvRunAttemptStopReason::PipelineContractFailure,
            true,
            "pipeline.contract",
            "decode output did not satisfy the buffer contract",
        },
        {
            ssv::SsvExitCode::Success,
            ssv::SsvRunAttemptStopReason::EndOfStream,
            true,
            {},
            {},
        },
    };
    auto config = make_config();
    config.sources.front().decode.device = {
        ssv::SsvDecodeDeviceKind::Drm,
        "/dev/dri/renderD129",
    };
    auto log_state = std::make_shared<RecordedLogState>();
    auto event_log = ssv::SsvEventLog::create(
        {}, std::make_unique<RecordingLogSink>(log_state));
    auto runner = ssv::ssv_runner_create_with_factory(
        std::move(config),
        *event_log,
        std::make_unique<FakeRunAttemptFactory>(
            state, make_auto_decode_capabilities()));

    const auto result = runner->run();

    assert(result.exit_code == ssv::SsvExitCode::Success);
    assert(state->attempts.size() == 2);
    const auto &accelerated = state->attempts[0];
    assert(accelerated.effective_config.sources.front().decode.mode
        == ssv::SsvDecodeMode::Auto);
    assert(accelerated.effective_config.sources.front().decode.device.kind
        == ssv::SsvDecodeDeviceKind::Drm);
    assert(accelerated.effective_config.sources.front().decode.device.value
        == "/dev/dri/renderD129");
    assert(accelerated.plan.decode.backend == ssv::SsvDecodeBackend::Vaapi);
    assert(accelerated.context.run_attempt_id == 1);
    const auto &software = state->attempts[1];
    assert(software.effective_config.sources.front().decode.mode
        == ssv::SsvDecodeMode::Software);
    assert(software.effective_config.sources.front().decode.device.kind
        == ssv::SsvDecodeDeviceKind::Auto);
    assert(software.effective_config.sources.front().decode.device.value
        == "auto");
    assert(software.plan.decode.backend == ssv::SsvDecodeBackend::Software);
    assert(software.context.run_attempt_id == 2);
    assert(state->lifecycle == std::vector<std::string>({
        "create:1",
        "run:1",
        "destroy:1",
        "create:2",
        "run:2",
        "destroy:2",
    }));
    assert(log_state->records.size() == 1);
    const auto &record = log_state->records.front().bytes;
    assert(record.find("event=acceleration_fallback")
        != std::string::npos);
    assert(record.find("run_attempt_id=1") != std::string::npos);
    assert(record.find("from=vaapi to=software")
        != std::string::npos);
    assert(record.find("stage=pipeline.contract")
        != std::string::npos);
}

void test_explicit_display_backend_does_not_fallback()
{
    auto state = std::make_shared<FakeFactoryState>();
    state->scripted_results.push_back({
        ssv::SsvExitCode::DisplayInitializationFailed,
        ssv::SsvRunAttemptStopReason::RuntimeError,
        false,
        "display.initialize",
        "failed to create GL context",
    });
    auto config = make_config();
    config.display.enabled = true;
    config.display.backend = ssv::SsvDisplayBackend::GtkGlSink;
    auto log_state = std::make_shared<RecordedLogState>();
    auto event_log = ssv::SsvEventLog::create(
        {}, std::make_unique<RecordingLogSink>(log_state));
    auto runner = ssv::ssv_runner_create_with_factory(
        std::move(config),
        *event_log,
        std::make_unique<FakeRunAttemptFactory>(
            state, make_accelerated_display_capabilities()));

    const auto result = runner->run();

    assert(result.exit_code
        == ssv::SsvExitCode::DisplayInitializationFailed);
    assert(result.stage == "display.initialize");
    assert(state->attempts.size() == 1);
    assert(!state->attempts.front().plan.display_fallback_allowed);
    assert(state->lifecycle == std::vector<std::string>({
        "create:1",
        "run:1",
        "destroy:1",
    }));
    assert(log_state->records.empty());
}

void test_explicit_decode_backend_does_not_fallback()
{
    auto state = std::make_shared<FakeFactoryState>();
    state->scripted_results.push_back({
        ssv::SsvExitCode::PipelineContractFailed,
        ssv::SsvRunAttemptStopReason::PipelineContractFailure,
        true,
        "pipeline.contract",
        "decode output did not satisfy the buffer contract",
    });
    auto config = make_config();
    config.sources.front().decode.mode = ssv::SsvDecodeMode::Vaapi;
    auto log_state = std::make_shared<RecordedLogState>();
    auto event_log = ssv::SsvEventLog::create(
        {}, std::make_unique<RecordingLogSink>(log_state));
    auto runner = ssv::ssv_runner_create_with_factory(
        std::move(config),
        *event_log,
        std::make_unique<FakeRunAttemptFactory>(
            state, make_accelerated_display_capabilities()));

    const auto result = runner->run();

    assert(result.exit_code == ssv::SsvExitCode::PipelineContractFailed);
    assert(result.stage == "pipeline.contract");
    assert(state->attempts.size() == 1);
    assert(!state->attempts.front().plan.decode.software_fallback_allowed);
    assert(state->lifecycle == std::vector<std::string>({
        "create:1",
        "run:1",
        "destroy:1",
    }));
    assert(log_state->records.empty());
}

void test_auto_display_creation_error_retries_with_gtk_sink()
{
    auto state = std::make_shared<FakeFactoryState>();
    state->scripted_results = {
        {},
        {
            ssv::SsvExitCode::Success,
            ssv::SsvRunAttemptStopReason::EndOfStream,
            true,
            {},
            {},
        },
    };
    state->scripted_creation_errors = {
        std::make_exception_ptr(ssv::SsvDisplayWindowError(
            "display.window", "failed to initialize GTK window")),
        {},
    };
    auto config = make_config();
    config.display.enabled = true;
    config.display.backend = ssv::SsvDisplayBackend::Auto;
    auto log_state = std::make_shared<RecordedLogState>();
    auto event_log = ssv::SsvEventLog::create(
        {}, std::make_unique<RecordingLogSink>(log_state));
    auto runner = ssv::ssv_runner_create_with_factory(
        std::move(config),
        *event_log,
        std::make_unique<FakeRunAttemptFactory>(
            state, make_accelerated_display_capabilities()));

    const auto result = runner->run();

    assert(result.exit_code == ssv::SsvExitCode::Success);
    assert(state->attempts.size() == 2);
    assert(state->attempts[0].context.run_attempt_id == 1);
    assert(state->attempts[1].effective_config.display.backend
        == ssv::SsvDisplayBackend::GtkSink);
    assert(state->attempts[1].context.run_attempt_id == 2);
    assert(state->lifecycle == std::vector<std::string>({
        "create:1",
        "create:2",
        "run:2",
        "destroy:2",
    }));
    assert(log_state->records.size() == 1);
    const auto &record = log_state->records.front().bytes;
    assert(record.find("run_attempt_id=1") != std::string::npos);
    assert(record.find("from=gtkglsink to=gtksink")
        != std::string::npos);
    assert(record.find("stage=display.window") != std::string::npos);
}

void test_auto_decode_pipeline_creation_error_retries_in_software()
{
    auto state = std::make_shared<FakeFactoryState>();
    state->scripted_results = {
        {},
        {
            ssv::SsvExitCode::Success,
            ssv::SsvRunAttemptStopReason::EndOfStream,
            true,
            {},
            {},
        },
    };
    state->scripted_creation_errors = {
        std::make_exception_ptr(ssv::SsvPipelineBuilderError(
            ssv::SsvExitCode::CapabilityUnavailable,
            "pipeline.build.decode",
            "failed to create the VAAPI decoder")),
        {},
    };
    auto config = make_config();
    config.sources.front().decode.device = {
        ssv::SsvDecodeDeviceKind::Drm,
        "/dev/dri/renderD129",
    };
    auto log_state = std::make_shared<RecordedLogState>();
    auto event_log = ssv::SsvEventLog::create(
        {}, std::make_unique<RecordingLogSink>(log_state));
    auto runner = ssv::ssv_runner_create_with_factory(
        std::move(config),
        *event_log,
        std::make_unique<FakeRunAttemptFactory>(
            state, make_auto_decode_capabilities()));

    const auto result = runner->run();

    assert(result.exit_code == ssv::SsvExitCode::Success);
    assert(state->attempts.size() == 2);
    assert(state->attempts[0].plan.decode.backend
        == ssv::SsvDecodeBackend::Vaapi);
    assert(state->attempts[0].context.run_attempt_id == 1);
    assert(state->attempts[1].effective_config.sources.front().decode.mode
        == ssv::SsvDecodeMode::Software);
    assert(state->attempts[1].plan.decode.backend
        == ssv::SsvDecodeBackend::Software);
    assert(state->attempts[1].context.run_attempt_id == 2);
    assert(state->lifecycle == std::vector<std::string>({
        "create:1",
        "create:2",
        "run:2",
        "destroy:2",
    }));
    assert(log_state->records.size() == 1);
    const auto &record = log_state->records.front().bytes;
    assert(record.find("run_attempt_id=1") != std::string::npos);
    assert(record.find("from=vaapi to=software")
        != std::string::npos);
    assert(record.find("stage=pipeline.build.decode")
        != std::string::npos);
}

void test_inference_creation_error_is_classified_without_fallback()
{
    auto state = std::make_shared<FakeFactoryState>();
    state->scripted_results = {{}};
    state->scripted_creation_errors = {
        std::make_exception_ptr(ssv::infer::SsvInferenceServiceError(
            "model.initialize", "failed to load the ONNX model")),
    };
    auto log_state = std::make_shared<RecordedLogState>();
    auto event_log = ssv::SsvEventLog::create(
        {}, std::make_unique<RecordingLogSink>(log_state));
    auto config = make_config();
    config.sources.front().decode.mode = ssv::SsvDecodeMode::Software;
    auto runner = ssv::ssv_runner_create_with_factory(
        std::move(config),
        *event_log,
        std::make_unique<FakeRunAttemptFactory>(
            state, make_software_capabilities()));

    const auto result = runner->run();

    assert(result.exit_code == ssv::SsvExitCode::ModelInitializationFailed);
    assert(result.stage == "model.initialize");
    assert(result.error == "failed to load the ONNX model");
    assert(state->attempts.size() == 1);
    assert(state->attempts.front().context.run_attempt_id == 1);
    assert(state->lifecycle == std::vector<std::string>({"create:1"}));
    assert(log_state->records.empty());
}

void test_pipeline_plan_error_is_classified_before_create()
{
    auto state = std::make_shared<FakeFactoryState>();
    auto config = make_config();
    config.sources.clear();
    auto log_state = std::make_shared<RecordedLogState>();
    auto event_log = ssv::SsvEventLog::create(
        {}, std::make_unique<RecordingLogSink>(log_state));
    auto runner = ssv::ssv_runner_create_with_factory(
        std::move(config),
        *event_log,
        std::make_unique<FakeRunAttemptFactory>(
            state, make_software_capabilities()));

    const auto result = runner->run();

    assert(result.exit_code == ssv::SsvExitCode::InvalidConfiguration);
    assert(result.stage == "config");
    assert(result.error
        == "pipeline plan requires exactly one source with a non-empty id");
    assert(state->attempts.empty());
    assert(state->lifecycle.empty());
    assert(log_state->records.empty());
}

void test_system_factory_prepares_and_creates_headless_attempt()
{
    auto log_state = std::make_shared<RecordedLogState>();
    auto event_log = ssv::SsvEventLog::create(
        {}, std::make_unique<RecordingLogSink>(log_state));
    auto factory = ssv::ssv_system_run_attempt_factory(*event_log);
    auto config = make_config();
    config.sources.front().decode.mode = ssv::SsvDecodeMode::Software;

    const auto capabilities = factory->prepare_run(config);
    const auto plan = ssv::SsvPipelinePlan::resolve(config, capabilities);
    auto creation = factory->create(
        config,
        plan,
        {
            .source_id = plan.source_id,
            .run_attempt_id = 1,
        });

    assert(!capabilities.gstreamer_elements.empty());
    assert(creation.attempt != nullptr);
    assert(creation.events.empty());
    assert(log_state->records.empty());
}

void test_factory_creation_events_are_emitted_for_the_attempt()
{
    auto state = std::make_shared<FakeFactoryState>();
    state->scripted_results.push_back({
        ssv::SsvExitCode::Success,
        ssv::SsvRunAttemptStopReason::EndOfStream,
        true,
        {},
        {},
    });
    state->scripted_creation_events = {{
        {
            .context = {},
            .payload = ssv::SsvAccelerationFallbackEvent {
                .from = "tensorrt",
                .to = "cuda",
                .stage = "provider.initialize",
                .reason = "TensorRT rejected the model",
            },
        },
    }};
    auto log_state = std::make_shared<RecordedLogState>();
    auto event_log = ssv::SsvEventLog::create(
        {}, std::make_unique<RecordingLogSink>(log_state));
    auto config = make_config();
    config.sources.front().decode.mode = ssv::SsvDecodeMode::Software;
    auto runner = ssv::ssv_runner_create_with_factory(
        std::move(config),
        *event_log,
        std::make_unique<FakeRunAttemptFactory>(
            state, make_software_capabilities()));

    const auto result = runner->run();

    assert(result.exit_code == ssv::SsvExitCode::Success);
    assert(log_state->records.size() == 1);
    const auto &record = log_state->records.front().bytes;
    assert(record.find("event=acceleration_fallback")
        != std::string::npos);
    assert(record.find("run_attempt_id=1") != std::string::npos);
    assert(record.find("from=tensorrt to=cuda") != std::string::npos);
    assert(record.find("stage=provider.initialize")
        != std::string::npos);
}

void test_retried_factory_does_not_repeat_provider_fallback_event()
{
    auto state = std::make_shared<FakeFactoryState>();
    state->scripted_results = {
        {
            ssv::SsvExitCode::DisplayInitializationFailed,
            ssv::SsvRunAttemptStopReason::RuntimeError,
            false,
            "display.initialize",
            "failed to create GL context",
        },
        {
            ssv::SsvExitCode::Success,
            ssv::SsvRunAttemptStopReason::EndOfStream,
            true,
            {},
            {},
        },
    };
    const auto provider_fallback = [] {
        return ssv::SsvEvent {
            .context = {},
            .payload = ssv::SsvAccelerationFallbackEvent {
                .from = "tensorrt",
                .to = "cuda",
                .stage = "provider.initialize",
                .reason = "TensorRT rejected the model",
            },
        };
    };
    state->scripted_creation_events = {
        {provider_fallback()},
        {provider_fallback()},
    };
    auto config = make_config();
    config.display.enabled = true;
    auto log_state = std::make_shared<RecordedLogState>();
    auto event_log = ssv::SsvEventLog::create(
        {}, std::make_unique<RecordingLogSink>(log_state));
    auto runner = ssv::ssv_runner_create_with_factory(
        std::move(config),
        *event_log,
        std::make_unique<FakeRunAttemptFactory>(
            state, make_accelerated_display_capabilities()));

    const auto result = runner->run();

    assert(result.exit_code == ssv::SsvExitCode::Success);
    assert(log_state->records.size() == 2);
    std::size_t provider_event_count = 0;
    std::size_t display_event_count = 0;
    for (const auto &record : log_state->records) {
        if (record.bytes.find("from=tensorrt to=cuda")
            != std::string::npos) {
            ++provider_event_count;
        }
        if (record.bytes.find("from=gtkglsink to=gtksink")
            != std::string::npos) {
            ++display_event_count;
        }
    }
    assert(provider_event_count == 1);
    assert(display_event_count == 1);
}

void test_plan_decode_fallback_is_emitted_for_the_attempt()
{
    auto state = std::make_shared<FakeFactoryState>();
    state->scripted_results.push_back({
        ssv::SsvExitCode::Success,
        ssv::SsvRunAttemptStopReason::EndOfStream,
        true,
        {},
        {},
    });
    auto log_state = std::make_shared<RecordedLogState>();
    auto event_log = ssv::SsvEventLog::create(
        {}, std::make_unique<RecordingLogSink>(log_state));
    auto runner = ssv::ssv_runner_create_with_factory(
        make_config(),
        *event_log,
        std::make_unique<FakeRunAttemptFactory>(
            state, make_nvdec_fallback_capabilities()));

    const auto result = runner->run();

    assert(result.exit_code == ssv::SsvExitCode::Success);
    assert(state->attempts.size() == 1);
    assert(state->attempts.front().plan.decode.backend
        == ssv::SsvDecodeBackend::Nvdec);
    assert(log_state->records.size() == 1);
    const auto &record = log_state->records.front().bytes;
    assert(record.find("run_attempt_id=1") != std::string::npos);
    assert(record.find("from=vaapi to=nvdec") != std::string::npos);
    assert(record.find("stage=decode.resolve") != std::string::npos);
}

void test_plan_display_fallback_reasons_are_emitted_once()
{
    auto state = std::make_shared<FakeFactoryState>();
    state->scripted_results.push_back({
        ssv::SsvExitCode::Success,
        ssv::SsvRunAttemptStopReason::EndOfStream,
        true,
        {},
        {},
    });
    auto config = make_config();
    config.sources.front().decode.mode = ssv::SsvDecodeMode::Software;
    config.display.enabled = true;
    auto log_state = std::make_shared<RecordedLogState>();
    auto event_log = ssv::SsvEventLog::create(
        {}, std::make_unique<RecordingLogSink>(log_state));
    auto runner = ssv::ssv_runner_create_with_factory(
        std::move(config),
        *event_log,
        std::make_unique<FakeRunAttemptFactory>(
            state, make_gtk_system_memory_capabilities()));

    const auto result = runner->run();

    assert(result.exit_code == ssv::SsvExitCode::Success);
    assert(state->attempts.size() == 1);
    assert(state->attempts.front().plan.display_backend
        == ssv::SsvResolvedDisplayBackend::GtkSink);
    assert(log_state->records.size() == 1);
    const auto &record = log_state->records.front().bytes;
    assert(record.find("run_attempt_id=1") != std::string::npos);
    assert(record.find("from=gtkglsink to=gtksink")
        != std::string::npos);
    assert(record.find("stage=display.resolve") != std::string::npos);
    assert(record.find("gtkglsink requires DMABuf input")
        != std::string::npos);
    assert(record.find("missing GStreamer element: gtkglsink")
        != std::string::npos);
}

void test_prepare_display_error_is_classified()
{
    auto state = std::make_shared<FakeFactoryState>();
    state->prepare_error = std::make_exception_ptr(
        ssv::SsvDisplayWindowError(
            "display.initialize", "GTK could not be initialized"));
    auto log_state = std::make_shared<RecordedLogState>();
    auto event_log = ssv::SsvEventLog::create(
        {}, std::make_unique<RecordingLogSink>(log_state));
    auto runner = ssv::ssv_runner_create_with_factory(
        make_config(),
        *event_log,
        std::make_unique<FakeRunAttemptFactory>(
            state, make_software_capabilities()));

    const auto result = runner->run();

    assert(result.exit_code
        == ssv::SsvExitCode::DisplayInitializationFailed);
    assert(result.stage == "display.initialize");
    assert(result.error == "GTK could not be initialized");
    assert(state->attempts.empty());
    assert(state->lifecycle.empty());
    assert(log_state->records.empty());
}

void test_unexpected_prepare_error_keeps_pipeline_start_classification()
{
    auto state = std::make_shared<FakeFactoryState>();
    state->prepare_error = std::make_exception_ptr(
        std::runtime_error("unexpected adapter failure"));
    auto log_state = std::make_shared<RecordedLogState>();
    auto event_log = ssv::SsvEventLog::create(
        {}, std::make_unique<RecordingLogSink>(log_state));
    auto runner = ssv::ssv_runner_create_with_factory(
        make_config(),
        *event_log,
        std::make_unique<FakeRunAttemptFactory>(
            state, make_software_capabilities()));

    const auto result = runner->run();

    assert(result.exit_code == ssv::SsvExitCode::PipelineContractFailed);
    assert(result.stage == "pipeline.start");
    assert(result.error == "unexpected adapter failure");
    assert(state->attempts.empty());
    assert(state->lifecycle.empty());
    assert(log_state->records.empty());
}

} // namespace

int main()
{
    test_successful_run_uses_one_owned_attempt_with_id_one();
    test_auto_display_failure_retries_after_destroying_failed_attempt();
    test_auto_decode_contract_failure_retries_in_software();
    test_explicit_display_backend_does_not_fallback();
    test_explicit_decode_backend_does_not_fallback();
    test_auto_display_creation_error_retries_with_gtk_sink();
    test_auto_decode_pipeline_creation_error_retries_in_software();
    test_inference_creation_error_is_classified_without_fallback();
    test_pipeline_plan_error_is_classified_before_create();
    test_system_factory_prepares_and_creates_headless_attempt();
    test_factory_creation_events_are_emitted_for_the_attempt();
    test_retried_factory_does_not_repeat_provider_fallback_event();
    test_plan_decode_fallback_is_emitted_for_the_attempt();
    test_plan_display_fallback_reasons_are_emitted_once();
    test_prepare_display_error_is_classified();
    test_unexpected_prepare_error_keeps_pipeline_start_classification();
    return 0;
}
