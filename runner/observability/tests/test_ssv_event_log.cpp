#include "observability/ssv_event_log.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

struct CapturingSinkState {
    std::vector<ssv::SsvEncodedLogRecord> records;
    ssv::SsvLogSinkStats stats;
    std::vector<std::chrono::steady_clock::time_point> flush_deadlines;
    std::vector<std::chrono::steady_clock::time_point> close_deadlines;
    std::vector<ssv::SsvLogSubmitResult> submit_results;
    ssv::SsvLogIoStatus flush_status = ssv::SsvLogIoStatus::Completed;
    ssv::SsvLogIoStatus close_status = ssv::SsvLogIoStatus::Completed;
    std::size_t flush_calls = 0;
    std::size_t close_calls = 0;
    std::size_t submit_calls = 0;
};

class CapturingSink final : public ssv::SsvLogSink {
public:
    explicit CapturingSink(std::shared_ptr<CapturingSinkState> state)
        : state_(std::move(state))
    {
    }

    ssv::SsvLogSubmitResult submit(
        ssv::SsvEncodedLogRecord record) noexcept override
    {
        state_->records.push_back(std::move(record));
        const auto result = state_->submit_calls < state_->submit_results.size()
            ? state_->submit_results[state_->submit_calls]
            : ssv::SsvLogSubmitResult::Accepted;
        ++state_->submit_calls;
        switch (result) {
        case ssv::SsvLogSubmitResult::Accepted:
            ++state_->stats.written_records;
            break;
        case ssv::SsvLogSubmitResult::DroppedBackpressure:
            ++state_->stats.async_dropped_records;
            break;
        case ssv::SsvLogSubmitResult::Failed:
            ++state_->stats.write_failed_records;
            break;
        }
        return result;
    }

    ssv::SsvLogIoResult flush(
        std::chrono::steady_clock::time_point deadline) noexcept override
    {
        ++state_->flush_calls;
        state_->flush_deadlines.push_back(deadline);
        return {state_->flush_status, state_->stats};
    }

    ssv::SsvLogIoResult close(
        std::chrono::steady_clock::time_point deadline) noexcept override
    {
        ++state_->close_calls;
        state_->close_deadlines.push_back(deadline);
        return {state_->close_status, state_->stats};
    }

private:
    std::shared_ptr<CapturingSinkState> state_;
};

struct BackpressuredSinkState {
    std::vector<ssv::SsvEncodedLogRecord> bypassed_records;
    ssv::SsvLogSinkStats stats;
    std::size_t synchronous_bypass_calls = 0;
    std::size_t flush_calls = 0;
    std::size_t close_calls = 0;
};

class BackpressuredSink final : public ssv::SsvLogSink {
public:
    explicit BackpressuredSink(std::shared_ptr<BackpressuredSinkState> state)
        : state_(std::move(state))
    {
    }

    ssv::SsvLogSubmitResult submit(
        ssv::SsvEncodedLogRecord record) noexcept override
    {
        if (!record.force_flush) {
            ++state_->stats.async_dropped_records;
            return ssv::SsvLogSubmitResult::DroppedBackpressure;
        }

        ++state_->synchronous_bypass_calls;
        state_->bypassed_records.push_back(std::move(record));
        ++state_->stats.written_records;
        return ssv::SsvLogSubmitResult::Accepted;
    }

    ssv::SsvLogIoResult flush(
        std::chrono::steady_clock::time_point) noexcept override
    {
        ++state_->flush_calls;
        return {ssv::SsvLogIoStatus::Completed, state_->stats};
    }

    ssv::SsvLogIoResult close(
        std::chrono::steady_clock::time_point) noexcept override
    {
        ++state_->close_calls;
        return {ssv::SsvLogIoStatus::Completed, state_->stats};
    }

private:
    std::shared_ptr<BackpressuredSinkState> state_;
};

void test_repeated_fallback_events_are_submitted_in_order()
{
    auto sink_state = std::make_shared<CapturingSinkState>();
    auto log = ssv::SsvEventLog::create(
        {}, std::make_unique<CapturingSink>(sink_state));
    const ssv::SsvEvent event {
        .context = {
            .source_id = "camera-01",
            .run_attempt_id = std::nullopt,
        },
        .payload = ssv::SsvAccelerationFallbackEvent {
            .from = "gtkglsink",
            .to = "gtksink",
            .stage = "display.resolve",
            .reason = "missing GL context",
        },
    };

    log->emit(event);
    log->emit(event);
    const auto stats = log->close();

    assert(sink_state->records.size() == 2);
    assert(sink_state->records[0].sequence == 1);
    assert(sink_state->records[1].sequence == 2);
    for (const auto &record : sink_state->records) {
        assert(record.severity == ssv::SsvEventSeverity::Warning);
        assert(!record.force_flush);
        assert(record.bytes
            == "event=acceleration_fallback source_id=camera-01 "
               "from=gtkglsink to=gtksink stage=display.resolve "
               "reason=\"missing GL context\"\n");
    }
    assert(stats.emit_calls == 2);
    assert(stats.accepted_by_sink == 2);
    assert(stats.written_records == 2);
}

void test_context_and_free_text_are_escaped_as_one_physical_line()
{
    auto sink_state = std::make_shared<CapturingSinkState>();
    auto log = ssv::SsvEventLog::create(
        {}, std::make_unique<CapturingSink>(sink_state));

    log->emit({
        .context = {
            .source_id = "camera 01",
            .run_attempt_id = 7,
        },
        .payload = ssv::SsvAccelerationFallbackEvent {
            .from = "vaapi",
            .to = "software",
            .stage = "decode.resolve",
            .reason = "bad \"caps\"\\line\nnext\t\x01",
        },
    });
    static_cast<void>(log->close());

    assert(sink_state->records.size() == 1);
    assert(sink_state->records.front().bytes
        == "event=acceleration_fallback source_id=\"camera 01\" "
           "run_attempt_id=7 from=vaapi to=software stage=decode.resolve "
           "reason=\"bad \\\"caps\\\"\\\\line\\nnext\\t\\u0001\"\n");
}

void test_runtime_resolved_uses_context_then_stable_snapshot_fields()
{
    auto sink_state = std::make_shared<CapturingSinkState>();
    auto log = ssv::SsvEventLog::create(
        {}, std::make_unique<CapturingSink>(sink_state));

    log->emit({
        .context = {
            .source_id = "camera-01",
            .run_attempt_id = 3,
        },
        .payload = ssv::SsvRuntimeResolvedEvent {
            .decoder = "nvh264dec",
            .va_device = "not-applicable",
            .va_driver = "not-applicable",
            .decode_memory = "CUDAMemory",
            .vpp = "disabled",
            .display_backend = "gtkglsink",
            .egl_renderer = "unknown",
            .provider_chain = "TensorrtExecutionProvider,CPUExecutionProvider",
            .provider_device = "device:0/compute_capability:8.9",
            .precision = "fp16",
            .model_hash = "abc123",
            .input_contract = "rgba-u8-nhwc",
            .cache_status = "hit",
        },
    });
    static_cast<void>(log->close());

    assert(sink_state->records.size() == 1);
    assert(sink_state->records.front().severity
        == ssv::SsvEventSeverity::Info);
    assert(sink_state->records.front().bytes
        == "event=runtime_resolved source_id=camera-01 run_attempt_id=3 "
           "decoder=nvh264dec va_device=not-applicable "
           "va_driver=not-applicable decode_memory=CUDAMemory vpp=disabled "
           "display_backend=gtkglsink egl_renderer=unknown "
           "provider_chain=\"TensorrtExecutionProvider,CPUExecutionProvider\" "
           "provider_device=device:0/compute_capability:8.9 precision=fp16 "
           "model_hash=abc123 input_contract=rgba-u8-nhwc cache_status=hit\n");
}

void test_runtime_resolved_pretty_output_groups_and_aligns_fields()
{
    auto sink_state = std::make_shared<CapturingSinkState>();
    ssv::SsvEventLogOptions options;
    options.format = ssv::SsvEventLogFormat::Pretty;
    auto log = ssv::SsvEventLog::create(
        options, std::make_unique<CapturingSink>(sink_state));

    log->emit({
        .context = {
            .source_id = "camera-01",
            .run_attempt_id = 3,
        },
        .payload = ssv::SsvRuntimeResolvedEvent {
            .decoder = "nvh264dec",
            .va_device = "not-applicable",
            .va_driver = "not-applicable",
            .decode_memory = "CUDAMemory",
            .vpp = "disabled",
            .display_backend = "gtkglsink",
            .egl_renderer = "unknown",
            .provider_chain = "TensorrtExecutionProvider,CPUExecutionProvider",
            .provider_device = "device:0/compute_capability:8.9",
            .precision = "fp16",
            .model_hash = "abc123",
            .input_contract = "rgba-u8-nhwc",
            .cache_status = "hit",
        },
    });
    static_cast<void>(log->close());

    assert(sink_state->records.size() == 1);
    const auto &bytes = sink_state->records.front().bytes;
    assert(bytes.starts_with(
        "event=runtime_resolved source_id=camera-01 run_attempt_id=3\n"));
    assert(bytes.find(
               "  decode:\n"
               "    decoder         = nvh264dec\n"
               "    va_device       = not-applicable\n"
               "    va_driver       = not-applicable\n"
               "    decode_memory   = CUDAMemory\n"
               "    vpp             = disabled\n")
        != std::string::npos);
    assert(bytes.find(
               "  display:\n"
               "    display_backend = gtkglsink\n"
               "    egl_renderer    = unknown\n")
        != std::string::npos);
    assert(bytes.find(
               "  inference:\n"
               "    provider_chain  = \"TensorrtExecutionProvider,CPUExecutionProvider\"\n"
               "    provider_device = device:0/compute_capability:8.9\n"
               "    precision       = fp16\n")
        != std::string::npos);
    assert(bytes.ends_with(
        "  model:\n"
        "    model_hash      = abc123\n"
        "    input_contract  = rgba-u8-nhwc\n"
        "    cache_status    = hit\n"));
}

void test_pretty_output_falls_back_when_display_would_exceed_limit()
{
    auto sink_state = std::make_shared<CapturingSinkState>();
    ssv::SsvEventLogOptions options;
    options.format = ssv::SsvEventLogFormat::Pretty;
    options.max_record_bytes = 1024;
    auto log = ssv::SsvEventLog::create(
        options, std::make_unique<CapturingSink>(sink_state));
    const std::string value(50, 'x');

    log->emit({
        .context = {
            .source_id = "camera",
            .run_attempt_id = 1,
        },
        .payload = ssv::SsvRuntimeResolvedEvent {
            .decoder = value,
            .va_device = value,
            .va_driver = value,
            .decode_memory = value,
            .vpp = value,
            .display_backend = value,
            .egl_renderer = value,
            .provider_chain = value,
            .provider_device = value,
            .precision = value,
            .model_hash = value,
            .input_contract = value,
            .cache_status = value,
        },
    });
    const auto stats = log->close();

    assert(sink_state->records.size() == 1);
    assert(stats.truncated == 0);
    assert(sink_state->records.front().bytes.starts_with(
        "event=runtime_resolved source_id=camera run_attempt_id=1 "));
    assert(sink_state->records.front().bytes.find("\n")
        == sink_state->records.front().bytes.size() - 1);
    assert(sink_state->records.front().bytes.find("  decode:")
        == std::string::npos);
}

void test_buffer_contract_failure_preserves_contract_snapshot()
{
    auto sink_state = std::make_shared<CapturingSinkState>();
    auto log = ssv::SsvEventLog::create(
        {}, std::make_unique<CapturingSink>(sink_state));

    log->emit({
        .context = {
            .source_id = "camera-01",
            .run_attempt_id = 2,
        },
        .payload = ssv::SsvBufferContractFailedEvent {
            .stage = "preprocess.input",
            .expected_caps = "video/x-raw,format=RGBA",
            .expected_allocator = "system",
            .expected_memory = "SystemMemory",
            .actual_caps = "video/x-raw,format=NV12",
            .actual_allocator = "dmabuf",
            .actual_memory = "DMABuf",
            .reason = "pixel format mismatch",
        },
    });
    static_cast<void>(log->close());

    assert(sink_state->records.size() == 1);
    assert(sink_state->records.front().severity
        == ssv::SsvEventSeverity::Warning);
    assert(sink_state->records.front().bytes
        == "event=buffer_contract_failed source_id=camera-01 run_attempt_id=2 "
           "stage=preprocess.input "
           "expected_caps=\"video/x-raw,format=RGBA\" "
           "expected_allocator=system expected_memory=SystemMemory "
           "actual_caps=\"video/x-raw,format=NV12\" "
           "actual_allocator=dmabuf actual_memory=DMABuf "
           "reason=\"pixel format mismatch\"\n");
}

void test_inference_stats_groups_throughput_and_millisecond_latency()
{
    auto sink_state = std::make_shared<CapturingSinkState>();
    auto log = ssv::SsvEventLog::create(
        {}, std::make_unique<CapturingSink>(sink_state));

    log->emit({
        .context = {
            .source_id = "local-test",
            .run_attempt_id = 1,
        },
        .payload = ssv::SsvInferenceStatsEvent {
            .received = 75,
            .dropped = 0,
            .completed = 75,
            .completed_fps = 14.9984,
            .longest_result_gap = std::chrono::microseconds {90297},
            .queue = {
                .p50 = std::chrono::microseconds {65},
                .p95 = std::chrono::microseconds {113},
            },
            .backend_execution = {
                .p50 = std::chrono::microseconds {2941},
                .p95 = std::chrono::microseconds {4146},
            },
            .backend_d2h = {},
            .postprocess = {
                .p50 = std::chrono::microseconds {39},
                .p95 = std::chrono::microseconds {49},
            },
            .total = {
                .p50 = std::chrono::microseconds {3046},
                .p95 = std::chrono::microseconds {4285},
            },
        },
    });
    static_cast<void>(log->close());

    assert(sink_state->records.size() == 1);
    assert(sink_state->records.front().severity
        == ssv::SsvEventSeverity::Info);
    assert(sink_state->records.front().bytes
        == "event=inference_stats source_id=local-test run_attempt_id=1 "
           "frames=75/75 dropped=0 fps=14.998 max_gap_ms=90.297 "
           "latency_ms=\"p50/p95 queue=0.065/0.113 decode=unmeasured "
           "color_resize=unmeasured normalize_layout=unmeasured "
           "backend_h2d=unmeasured backend_execution=2.941/4.146 "
           "backend_d2h=unmeasured backend_unattributed=unmeasured "
           "postprocess=0.039/0.049 "
           "total=3.046/4.285\"\n");
}

void test_inference_stats_pretty_output_aligns_latency_percentiles()
{
    auto sink_state = std::make_shared<CapturingSinkState>();
    ssv::SsvEventLogOptions options;
    options.format = ssv::SsvEventLogFormat::Pretty;
    auto log = ssv::SsvEventLog::create(
        options, std::make_unique<CapturingSink>(sink_state));

    log->emit({
        .context = {
            .source_id = "local-test",
            .run_attempt_id = 1,
        },
        .payload = ssv::SsvInferenceStatsEvent {
            .received = 34,
            .dropped = 0,
            .completed = 33,
            .completed_fps = 6.048,
            .longest_result_gap = std::chrono::microseconds {3'523'678},
            .queue = {
                .p50 = std::chrono::microseconds {45},
                .p95 = std::chrono::microseconds {84},
            },
            .backend_execution = {
                .p50 = std::chrono::microseconds {50'763},
                .p95 = std::chrono::microseconds {52'370},
            },
            .backend_d2h = {},
            .postprocess = {
                .p50 = std::chrono::microseconds {50},
                .p95 = std::chrono::microseconds {63},
            },
            .total = {
                .p50 = std::chrono::microseconds {50'873},
                .p95 = std::chrono::microseconds {52'509},
            },
        },
    });
    static_cast<void>(log->close());

    assert(sink_state->records.size() == 1);
    const auto &bytes = sink_state->records.front().bytes;
    assert(bytes.starts_with(
        "event=inference_stats source_id=local-test run_attempt_id=1\n"));
    assert(bytes.find(
               "  throughput:\n"
               "    frames          = 33/34\n"
               "    dropped         = 0\n"
               "    fps             = 6.048\n"
               "    max_gap_ms      = 3523.678\n")
        != std::string::npos);
    assert(bytes.find(
               "  latency_ms:\n"
               "    metric           p50       p95\n")
        != std::string::npos);
    assert(bytes.find("    queue            0.045    0.084\n")
        != std::string::npos);
    assert(bytes.find("    backend_execution  50.763   52.370\n")
        != std::string::npos);
    assert(bytes.ends_with("    total            50.873   52.509\n"));
}

void test_fatal_error_uses_fixed_code_and_force_flush_metadata()
{
    auto sink_state = std::make_shared<CapturingSinkState>();
    auto log = ssv::SsvEventLog::create(
        {}, std::make_unique<CapturingSink>(sink_state));

    log->emit({
        .context = {
            .source_id = "local-test",
            .run_attempt_id = std::nullopt,
        },
        .payload = ssv::SsvFatalErrorEvent {
            .exit_code = static_cast<ssv::SsvExitCode>(6),
            .stage = "runtime",
            .error = "Could not open resource",
        },
    });
    static_cast<void>(log->close());

    assert(sink_state->records.size() == 1);
    assert(sink_state->records.front().severity
        == ssv::SsvEventSeverity::Fatal);
    assert(sink_state->records.front().force_flush);
    assert(sink_state->records.front().bytes
        == "event=fatal_error source_id=local-test exit_code=6 "
           "stage=runtime error=\"Could not open resource\"\n");
}

void test_native_diagnostic_preserves_source_severity_and_optional_debug()
{
    auto sink_state = std::make_shared<CapturingSinkState>();
    auto log = ssv::SsvEventLog::create(
        {}, std::make_unique<CapturingSink>(sink_state));

    log->emit({
        .context = {
            .source_id = "local-test",
            .run_attempt_id = 4,
        },
        .payload = ssv::SsvNativeDiagnosticEvent {
            .origin = "gstreamer",
            .source_severity = ssv::SsvNativeDiagnosticSeverity::Warning,
            .domain = "gst-resource-error-quark",
            .code = 3,
            .message = "resource is slow",
            .debug = "gstbasesrc.c:123",
        },
    });
    log->emit({
        .context = {
            .source_id = "local-test",
            .run_attempt_id = 4,
        },
        .payload = ssv::SsvNativeDiagnosticEvent {
            .origin = "gstreamer",
            .source_severity = ssv::SsvNativeDiagnosticSeverity::Error,
            .domain = "gst-stream-error-quark",
            .code = 1,
            .message = "decode failed",
            .debug = std::nullopt,
        },
    });
    static_cast<void>(log->close());

    assert(sink_state->records.size() == 2);
    assert(sink_state->records[0].severity
        == ssv::SsvEventSeverity::Warning);
    assert(sink_state->records[0].bytes
        == "event=native_diagnostic source_id=local-test run_attempt_id=4 "
           "origin=gstreamer source_severity=warning "
           "domain=gst-resource-error-quark code=3 "
           "message=\"resource is slow\" debug=gstbasesrc.c:123\n");
    assert(sink_state->records[1].severity
        == ssv::SsvEventSeverity::Error);
    assert(sink_state->records[1].bytes
        == "event=native_diagnostic source_id=local-test run_attempt_id=4 "
           "origin=gstreamer source_severity=error "
           "domain=gst-stream-error-quark code=1 "
           "message=\"decode failed\"\n");
}

void test_create_rejects_invalid_options_and_null_sink()
{
    const auto expect_invalid = [](ssv::SsvEventLogOptions options,
                                    std::unique_ptr<ssv::SsvLogSink> sink) {
        bool rejected = false;
        try {
            auto log = ssv::SsvEventLog::create(options, std::move(sink));
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        assert(rejected);
    };
    const auto make_sink = [] {
        return std::make_unique<CapturingSink>(
            std::make_shared<CapturingSinkState>());
    };

    expect_invalid({}, {});

    ssv::SsvEventLogOptions options;
    options.max_record_bytes = 1023;
    expect_invalid(options, make_sink());

    options = {};
    options.fatal_flush_timeout = std::chrono::milliseconds {0};
    expect_invalid(options, make_sink());

    options = {};
    options.shutdown_timeout = std::chrono::milliseconds {-1};
    expect_invalid(options, make_sink());

    options = {};
    options.fatal_flush_timeout = std::chrono::milliseconds {1001};
    options.shutdown_timeout = std::chrono::milliseconds {1000};
    expect_invalid(options, make_sink());
}

void test_filtering_happens_before_encoding_and_sequence_assignment()
{
    auto sink_state = std::make_shared<CapturingSinkState>();
    ssv::SsvEventLogOptions options;
    options.minimum_severity = ssv::SsvEventSeverity::Error;
    auto log = ssv::SsvEventLog::create(
        options, std::make_unique<CapturingSink>(sink_state));

    log->emit({
        .context = {.source_id = "camera", .run_attempt_id = 1},
        .payload = ssv::SsvInferenceStatsEvent {},
    });
    log->emit({
        .context = {.source_id = "camera", .run_attempt_id = 1},
        .payload = ssv::SsvAccelerationFallbackEvent {
            .from = "gpu",
            .to = "cpu",
            .stage = "decode",
            .reason = "unavailable",
        },
    });
    log->emit({
        .context = {.source_id = "camera", .run_attempt_id = 1},
        .payload = ssv::SsvNativeDiagnosticEvent {
            .origin = "gstreamer",
            .source_severity = ssv::SsvNativeDiagnosticSeverity::Error,
            .domain = "gst-stream-error-quark",
            .code = 1,
            .message = "decode failed",
            .debug = std::nullopt,
        },
    });
    log->emit({
        .context = {.source_id = "camera", .run_attempt_id = 1},
        .payload = ssv::SsvFatalErrorEvent {
            .exit_code = static_cast<ssv::SsvExitCode>(6),
            .stage = "runtime",
            .error = "stopped",
        },
    });
    const auto stats = log->close();

    assert(sink_state->records.size() == 2);
    assert(sink_state->records[0].sequence == 1);
    assert(sink_state->records[0].severity == ssv::SsvEventSeverity::Error);
    assert(sink_state->records[1].sequence == 2);
    assert(sink_state->records[1].severity == ssv::SsvEventSeverity::Fatal);
    assert(stats.emit_calls == 4);
    assert(stats.filtered == 2);
    assert(stats.accepted_by_sink == 2);
}

void test_oversized_free_text_is_truncated_without_line_or_utf8_breakage()
{
    auto sink_state = std::make_shared<CapturingSinkState>();
    ssv::SsvEventLogOptions options;
    options.max_record_bytes = 1024;
    auto log = ssv::SsvEventLog::create(
        options, std::make_unique<CapturingSink>(sink_state));
    std::string reason;
    for (int index = 0; index < 2'000; ++index)
        reason += "é";
    reason += "\nforged=true";

    log->emit({
        .context = {.source_id = "camera", .run_attempt_id = 1},
        .payload = ssv::SsvAccelerationFallbackEvent {
            .from = "gpu",
            .to = "cpu",
            .stage = "decode.resolve",
            .reason = std::move(reason),
        },
    });
    const auto stats = log->close();

    assert(sink_state->records.size() == 1);
    const auto &bytes = sink_state->records.front().bytes;
    assert(bytes.size() <= options.max_record_bytes);
    assert(bytes.starts_with(
        "event=acceleration_fallback source_id=camera run_attempt_id=1 "
        "from=gpu to=cpu stage=decode.resolve reason="));
    assert(bytes.find(" record_truncated=true omitted_bytes=")
        != std::string::npos);
    assert(bytes.find("\nforged=true") == std::string::npos);
    assert(bytes.find("\xc3\"") == std::string::npos);
    std::size_t physical_newlines = 0;
    for (const char character : bytes) {
        if (character == '\n')
            ++physical_newlines;
    }
    assert(physical_newlines == 1);
    assert(bytes.back() == '\n');
    assert(stats.truncated == 1);
}

void test_fatal_forces_flush_with_its_own_deadline()
{
    auto sink_state = std::make_shared<CapturingSinkState>();
    sink_state->flush_status = ssv::SsvLogIoStatus::TimedOut;
    ssv::SsvEventLogOptions options;
    options.fatal_flush_timeout = std::chrono::milliseconds {50};
    options.shutdown_timeout = std::chrono::milliseconds {100};
    auto log = ssv::SsvEventLog::create(
        options, std::make_unique<CapturingSink>(sink_state));

    const auto before_emit = std::chrono::steady_clock::now();
    log->emit({
        .context = {.source_id = "camera", .run_attempt_id = 1},
        .payload = ssv::SsvFatalErrorEvent {
            .exit_code = static_cast<ssv::SsvExitCode>(6),
            .stage = "runtime",
            .error = "stopped",
        },
    });
    const auto after_emit = std::chrono::steady_clock::now();
    const auto stats = log->close();

    assert(sink_state->flush_calls == 1);
    assert(sink_state->flush_deadlines.size() == 1);
    assert(sink_state->flush_deadlines.front()
        >= before_emit + options.fatal_flush_timeout);
    assert(sink_state->flush_deadlines.front()
        <= after_emit + options.fatal_flush_timeout);
    assert(sink_state->close_calls == 1);
    assert(stats.final_flush_status == ssv::SsvLogIoStatus::TimedOut);
    assert(stats.final_close_status == ssv::SsvLogIoStatus::Completed);
}

void test_fatal_uses_synchronous_bypass_when_async_path_is_backpressured()
{
    auto sink_state = std::make_shared<BackpressuredSinkState>();
    auto log = ssv::SsvEventLog::create(
        {}, std::make_unique<BackpressuredSink>(sink_state));

    log->emit({
        .context = {.source_id = "camera", .run_attempt_id = 1},
        .payload = ssv::SsvAccelerationFallbackEvent {
            .from = "gpu",
            .to = "cpu",
            .stage = "decode",
            .reason = "unavailable",
        },
    });
    log->emit({
        .context = {.source_id = "camera", .run_attempt_id = 1},
        .payload = ssv::SsvFatalErrorEvent {
            .exit_code = static_cast<ssv::SsvExitCode>(6),
            .stage = "runtime",
            .error = "stopped",
        },
    });
    const auto stats = log->close();

    assert(sink_state->synchronous_bypass_calls == 1);
    assert(sink_state->bypassed_records.size() == 1);
    assert(sink_state->bypassed_records.front().force_flush);
    assert(sink_state->bypassed_records.front().bytes.starts_with(
        "event=fatal_error source_id=camera run_attempt_id=1 "));
    assert(sink_state->flush_calls == 1);
    assert(sink_state->close_calls == 1);
    assert(stats.emit_calls == 2);
    assert(stats.accepted_by_sink == 1);
    assert(stats.submit_dropped == 1);
    assert(stats.written_records == 1);
    assert(stats.async_dropped_records == 1);
    assert(stats.emergency_write_attempts == 0);
}

void test_submit_results_and_sink_totals_are_counted_separately()
{
    auto sink_state = std::make_shared<CapturingSinkState>();
    sink_state->submit_results = {
        ssv::SsvLogSubmitResult::Accepted,
        ssv::SsvLogSubmitResult::DroppedBackpressure,
        ssv::SsvLogSubmitResult::Failed,
    };
    auto log = ssv::SsvEventLog::create(
        {}, std::make_unique<CapturingSink>(sink_state));
    const ssv::SsvEvent event {
        .context = {.source_id = "camera", .run_attempt_id = 1},
        .payload = ssv::SsvAccelerationFallbackEvent {
            .from = "gpu",
            .to = "cpu",
            .stage = "decode",
            .reason = "unavailable",
        },
    };

    log->emit(event);
    log->emit(event);
    log->emit(event);
    const auto stats = log->close();

    assert(stats.emit_calls == 3);
    assert(stats.accepted_by_sink == 1);
    assert(stats.submit_dropped == 1);
    assert(stats.submit_failed == 1);
    assert(stats.written_records == 1);
    assert(stats.async_dropped_records == 1);
    assert(stats.write_failed_records == 1);
    assert(stats.emit_calls
        == stats.filtered
            + stats.rejected_after_close
            + stats.encode_failed
            + stats.accepted_by_sink
            + stats.submit_dropped
            + stats.submit_failed);
}

void test_close_is_idempotent_and_late_emit_is_rejected()
{
    auto sink_state = std::make_shared<CapturingSinkState>();
    auto log = ssv::SsvEventLog::create(
        {}, std::make_unique<CapturingSink>(sink_state));

    const auto first = log->close();
    const auto second = log->close();
    assert(sink_state->close_calls == 1);
    assert(first.emit_calls == second.emit_calls);
    assert(first.final_close_status == second.final_close_status);

    log->emit({
        .context = {.source_id = "camera", .run_attempt_id = 1},
        .payload = ssv::SsvAccelerationFallbackEvent {
            .from = "gpu",
            .to = "cpu",
            .stage = "decode",
            .reason = "unavailable",
        },
    });
    const auto after_late_emit = log->close();
    assert(sink_state->close_calls == 1);
    assert(sink_state->records.empty());
    assert(after_late_emit.emit_calls == 1);
    assert(after_late_emit.rejected_after_close == 1);
}

void test_close_timeout_is_reported_with_shutdown_deadline()
{
    auto sink_state = std::make_shared<CapturingSinkState>();
    sink_state->close_status = ssv::SsvLogIoStatus::TimedOut;
    ssv::SsvEventLogOptions options;
    options.fatal_flush_timeout = std::chrono::milliseconds {10};
    options.shutdown_timeout = std::chrono::milliseconds {50};
    auto log = ssv::SsvEventLog::create(
        options, std::make_unique<CapturingSink>(sink_state));

    const auto before_close = std::chrono::steady_clock::now();
    const auto first = log->close();
    const auto after_close = std::chrono::steady_clock::now();
    const auto second = log->close();

    assert(sink_state->close_calls == 1);
    assert(sink_state->close_deadlines.size() == 1);
    assert(sink_state->close_deadlines.front()
        >= before_close + options.shutdown_timeout);
    assert(sink_state->close_deadlines.front()
        <= after_close + options.shutdown_timeout);
    assert(first.final_close_status == ssv::SsvLogIoStatus::TimedOut);
    assert(second.final_close_status == ssv::SsvLogIoStatus::TimedOut);
}

void test_concurrent_close_calls_close_sink_once()
{
    auto sink_state = std::make_shared<CapturingSinkState>();
    auto log = ssv::SsvEventLog::create(
        {}, std::make_unique<CapturingSink>(sink_state));
    constexpr std::size_t thread_count = 16;
    std::atomic<std::size_t> ready {0};
    std::atomic<bool> start {false};
    std::vector<ssv::SsvEventLogStats> snapshots(thread_count);
    std::vector<std::thread> threads;

    for (std::size_t index = 0; index < thread_count; ++index) {
        threads.emplace_back([&, index] {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            snapshots[index] = log->close();
        });
    }
    while (ready.load(std::memory_order_acquire) != thread_count)
        std::this_thread::yield();
    start.store(true, std::memory_order_release);
    for (auto &thread : threads)
        thread.join();

    assert(sink_state->close_calls == 1);
    for (const auto &snapshot : snapshots) {
        assert(snapshot.emit_calls == 0);
        assert(snapshot.final_close_status == ssv::SsvLogIoStatus::Completed);
    }
}

void test_emit_racing_close_is_fully_submitted_or_rejected()
{
    constexpr std::size_t attempt_count = 64;
    const ssv::SsvEvent event {
        .context = {.source_id = "camera", .run_attempt_id = 1},
        .payload = ssv::SsvAccelerationFallbackEvent {
            .from = "gpu",
            .to = "cpu",
            .stage = "decode",
            .reason = "unavailable",
        },
    };

    for (std::size_t attempt = 0; attempt < attempt_count; ++attempt) {
        auto sink_state = std::make_shared<CapturingSinkState>();
        auto log = ssv::SsvEventLog::create(
            {}, std::make_unique<CapturingSink>(sink_state));
        std::atomic<std::size_t> ready {0};
        std::atomic<bool> start {false};
        std::thread emitter([&] {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            log->emit(event);
        });
        std::thread closer([&] {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            static_cast<void>(log->close());
        });
        while (ready.load(std::memory_order_acquire) != 2)
            std::this_thread::yield();
        start.store(true, std::memory_order_release);
        emitter.join();
        closer.join();

        const auto stats = log->close();
        assert(sink_state->close_calls == 1);
        assert(stats.emit_calls == 1);
        assert(stats.accepted_by_sink + stats.rejected_after_close == 1);
        assert(stats.filtered == 0);
        assert(stats.encode_failed == 0);
        assert(stats.submit_dropped == 0);
        assert(stats.submit_failed == 0);
        assert(sink_state->records.size() == stats.accepted_by_sink);
        if (!sink_state->records.empty()) {
            assert(sink_state->records.front().sequence == 1);
            assert(sink_state->records.front().bytes
                == "event=acceleration_fallback source_id=camera "
                   "run_attempt_id=1 from=gpu to=cpu stage=decode "
                   "reason=unavailable\n");
        }
    }
}

void test_concurrent_emit_is_serialized_in_sink_sequence()
{
    auto sink_state = std::make_shared<CapturingSinkState>();
    auto log = ssv::SsvEventLog::create(
        {}, std::make_unique<CapturingSink>(sink_state));
    constexpr std::size_t thread_count = 8;
    constexpr std::size_t events_per_thread = 100;
    const ssv::SsvEvent event {
        .context = {.source_id = "camera", .run_attempt_id = 1},
        .payload = ssv::SsvAccelerationFallbackEvent {
            .from = "gpu",
            .to = "cpu",
            .stage = "decode",
            .reason = "unavailable",
        },
    };
    std::vector<std::thread> threads;
    for (std::size_t index = 0; index < thread_count; ++index) {
        threads.emplace_back([&] {
            for (std::size_t emitted = 0; emitted < events_per_thread; ++emitted)
                log->emit(event);
        });
    }
    for (auto &thread : threads)
        thread.join();
    const auto stats = log->close();

    const auto expected = thread_count * events_per_thread;
    assert(sink_state->records.size() == expected);
    for (std::size_t index = 0; index < sink_state->records.size(); ++index)
        assert(sink_state->records[index].sequence == index + 1);
    assert(stats.emit_calls == expected);
    assert(stats.accepted_by_sink == expected);
    assert(stats.written_records == expected);
}

void test_failed_fatal_delivery_attempts_one_minimal_stderr_write()
{
    int pipe_fds[2] {-1, -1};
    assert(::pipe(pipe_fds) == 0);
    const int saved_stderr = ::dup(STDERR_FILENO);
    assert(saved_stderr >= 0);
    assert(::dup2(pipe_fds[1], STDERR_FILENO) == STDERR_FILENO);

    auto sink_state = std::make_shared<CapturingSinkState>();
    sink_state->submit_results = {ssv::SsvLogSubmitResult::Failed};
    auto log = ssv::SsvEventLog::create(
        {}, std::make_unique<CapturingSink>(sink_state));
    log->emit({
        .context = {.source_id = "camera", .run_attempt_id = 1},
        .payload = ssv::SsvFatalErrorEvent {
            .exit_code = static_cast<ssv::SsvExitCode>(6),
            .stage = "runtime",
            .error = "stopped",
        },
    });
    const auto stats = log->close();

    assert(::dup2(saved_stderr, STDERR_FILENO) == STDERR_FILENO);
    assert(::close(saved_stderr) == 0);
    assert(::close(pipe_fds[1]) == 0);
    std::string captured;
    char buffer[128];
    while (true) {
        const auto received = ::read(pipe_fds[0], buffer, sizeof(buffer));
        if (received == 0)
            break;
        assert(received > 0);
        captured.append(buffer, static_cast<std::size_t>(received));
    }
    assert(::close(pipe_fds[0]) == 0);

    assert(captured == "event=fatal_error log_delivery_failed=true\n");
    assert(stats.submit_failed == 1);
    assert(stats.emergency_write_attempts == 1);
    assert(stats.emergency_write_failures == 0);
}

} // namespace

int main()
{
    test_repeated_fallback_events_are_submitted_in_order();
    test_context_and_free_text_are_escaped_as_one_physical_line();
    test_runtime_resolved_uses_context_then_stable_snapshot_fields();
    test_runtime_resolved_pretty_output_groups_and_aligns_fields();
    test_pretty_output_falls_back_when_display_would_exceed_limit();
    test_buffer_contract_failure_preserves_contract_snapshot();
    test_inference_stats_groups_throughput_and_millisecond_latency();
    test_inference_stats_pretty_output_aligns_latency_percentiles();
    test_fatal_error_uses_fixed_code_and_force_flush_metadata();
    test_native_diagnostic_preserves_source_severity_and_optional_debug();
    test_create_rejects_invalid_options_and_null_sink();
    test_filtering_happens_before_encoding_and_sequence_assignment();
    test_oversized_free_text_is_truncated_without_line_or_utf8_breakage();
    test_fatal_forces_flush_with_its_own_deadline();
    test_fatal_uses_synchronous_bypass_when_async_path_is_backpressured();
    test_submit_results_and_sink_totals_are_counted_separately();
    test_close_is_idempotent_and_late_emit_is_rejected();
    test_close_timeout_is_reported_with_shutdown_deadline();
    test_concurrent_close_calls_close_sink_once();
    test_emit_racing_close_is_fully_submitted_or_rejected();
    test_concurrent_emit_is_serialized_in_sink_sequence();
    test_failed_fatal_delivery_attempts_one_minimal_stderr_write();
    return 0;
}
