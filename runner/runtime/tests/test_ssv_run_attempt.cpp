#include "observability/ssv_event_log.hpp"
#include "pipeline/ssv_hardware_capabilities.hpp"
#include "pipeline/ssv_pipeline_contract.hpp"
#include "pipeline/ssv_pipeline_plan.hpp"
#include "runtime/ssv_run_attempt.hpp"
#include "ssv_inference_service.hpp"
#include "ssv_inference_test_service.hpp"

#include <gst/gst.h>

#include <cassert>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

using namespace std::chrono_literals;

void record_pipeline_finalized(gpointer data, GObject *)
{
    static_cast<std::vector<std::string> *>(data)->emplace_back(
        "pipeline.destroy");
}

class StringLogSink final : public ssv::SsvLogSink {
public:
    explicit StringLogSink(std::string &output) noexcept
        : output_(&output)
    {
    }

    ssv::SsvLogSubmitResult submit(
        ssv::SsvEncodedLogRecord record) noexcept override
    {
        output_->append(record.bytes);
        ++stats_.written_records;
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
    std::string *output_;
    ssv::SsvLogSinkStats stats_;
};

std::unique_ptr<ssv::SsvEventLog> make_event_log(std::string &output)
{
    return ssv::SsvEventLog::create(
        {}, std::make_unique<StringLogSink>(output));
}

std::size_t count_occurrences(
    std::string_view value,
    std::string_view needle)
{
    std::size_t count = 0;
    std::size_t offset = 0;
    while ((offset = value.find(needle, offset)) != std::string_view::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

ssv::SsvConfig make_headless_config()
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

ssv::SsvPipelinePlan make_headless_plan(const ssv::SsvConfig &config)
{
    ssv::SsvHardwareCapabilities capabilities;
    capabilities.gstreamer_elements = {"avdec_h264"};
    return ssv::SsvPipelinePlan::resolve(config, capabilities);
}

ssv::SsvPipelinePlan make_enabled_plan(const ssv::SsvConfig &config)
{
    ssv::SsvHardwareCapabilities capabilities;
    capabilities.gstreamer_elements = {
        "vah264dec",
        "vapostproc",
        "avdec_h264",
        "gtkglsink",
        "glupload",
        "glcolorconvert",
        "gtksink",
    };
    capabilities.onnxruntime_available = true;
    return ssv::SsvPipelinePlan::resolve(config, capabilities);
}

void post_fake_bus_error(GstElement *pipeline)
{
    GError *error = g_error_new_literal(
        g_quark_from_static_string("ssv-runner-test"),
        1,
        "fake bus error");
    GstMessage *message = gst_message_new_error(
        GST_OBJECT(pipeline), error, "injected by runner test");
    g_error_free(error);
    assert(gst_element_post_message(pipeline, message));
}

void post_fake_bus_warning(GstElement *pipeline)
{
    GError *error = g_error_new_literal(
        g_quark_from_static_string("ssv-runner-test"),
        2,
        "fake bus warning");
    GstMessage *message = gst_message_new_warning(
        GST_OBJECT(pipeline), error, "injected warning debug context");
    g_error_free(error);
    assert(gst_element_post_message(pipeline, message));
}

void post_resource_not_found_bus_error(
    GstElement *element,
    const char *message_text)
{
    GError *error = g_error_new_literal(
        GST_RESOURCE_ERROR,
        GST_RESOURCE_ERROR_NOT_FOUND,
        message_text);
    GstMessage *message = gst_message_new_error(
        GST_OBJECT(element), error, "injected gtkglsink resource failure");
    g_error_free(error);
    assert(gst_element_post_message(element, message));
}

ssv::SsvPipelineInstance adopt_pipeline(
    GstElement *pipeline,
    bool with_display_attachment = false)
{
    ssv::SsvPipelinePtr owned_pipeline(pipeline);
    if (!with_display_attachment)
        return ssv::SsvPipelineInstance(std::move(owned_pipeline));

    GstElement *sink = gst_bin_get_by_name(
        GST_BIN(pipeline), "display-sink");
    GstElement *timing_element = gst_bin_get_by_name(
        GST_BIN(pipeline), "display-sink-caps");
    assert(sink != nullptr && timing_element != nullptr);
    GstPad *timing_pad = gst_element_get_static_pad(timing_element, "src");
    assert(timing_pad != nullptr);
    ssv::SsvDisplayAttachment attachment(sink, timing_pad);
    gst_object_unref(timing_pad);
    gst_object_unref(timing_element);
    gst_object_unref(sink);
    return ssv::SsvPipelineInstance(
        std::move(owned_pipeline), std::move(attachment));
}

ssv::SsvPipelineInstance make_display_live_pipeline()
{
    GError *error = nullptr;
    GstElement *pipeline = gst_parse_launch(
        "videotestsrc is-live=true ! "
        "identity name=display-gl-upload ! "
        "identity name=display-sink-caps ! "
        "fakesink name=display-sink sync=false",
        &error);
    assert(error == nullptr);
    assert(pipeline != nullptr);
    return adopt_pipeline(pipeline, true);
}

void post_contract_bus_error(GstElement *pipeline)
{
    GError *error = g_error_new_literal(
        ssv::ssv_pipeline_contract_error_quark(),
        1,
        "fake memory contract failure");
    GstStructure *details = gst_structure_new(
        "ssv-buffer-contract-failed",
        "boundary", G_TYPE_INT,
        static_cast<int>(ssv::SsvPipelineBoundary::DecodeTee),
        "expected-caps", G_TYPE_STRING,
        "video/x-raw(memory:VAMemory),format=NV12",
        "expected-allocator", G_TYPE_STRING, "VAMemory",
        "expected-memory", G_TYPE_STRING, "VAMemory",
        "actual-caps", G_TYPE_STRING,
        "video/x-raw,format=NV12",
        "actual-allocator", G_TYPE_STRING, "SystemMemory",
        "actual-memory", G_TYPE_STRING, "SystemMemory",
        "message", G_TYPE_STRING, "fake memory contract failure",
        nullptr);
    GstMessage *message = gst_message_new_error_with_details(
        GST_OBJECT(pipeline),
        error,
        "injected contract failure",
        details);
    g_error_free(error);
    assert(gst_element_post_message(pipeline, message));
}

void post_contract_ready(
    GstElement *pipeline,
    ssv::SsvPipelineBoundary boundary)
{
    GstStructure *details = gst_structure_new(
        "ssv-buffer-contract-ready",
        "boundary", G_TYPE_INT, static_cast<int>(boundary),
        nullptr);
    assert(gst_element_post_message(
        pipeline,
        gst_message_new_element(GST_OBJECT(pipeline), details)));
}

ssv::SsvPipelineInstance make_live_pipeline()
{
    GError *error = nullptr;
    GstElement *pipeline = gst_parse_launch(
        "videotestsrc is-live=true ! fakesink sync=false",
        &error);
    assert(error == nullptr);
    assert(pipeline != nullptr);
    return adopt_pipeline(pipeline);
}

ssv::SsvPipelineInstance make_finite_pipeline()
{
    GError *error = nullptr;
    GstElement *pipeline = gst_parse_launch(
        "videotestsrc num-buffers=1 ! fakesink sync=false async=false",
        &error);
    assert(error == nullptr);
    assert(pipeline != nullptr);
    return adopt_pipeline(pipeline);
}

ssv::SsvPipelineInstance make_display_finite_pipeline()
{
    GError *error = nullptr;
    GstElement *pipeline = gst_parse_launch(
        "videotestsrc num-buffers=1 ! "
        "identity name=display-sink-caps ! "
        "fakesink name=display-sink sync=false async=false",
        &error);
    assert(error == nullptr);
    assert(pipeline != nullptr);
    return adopt_pipeline(pipeline, true);
}

ssv::SsvPipelineInstance make_slow_finite_pipeline()
{
    GError *error = nullptr;
    GstElement *pipeline = gst_parse_launch(
        "videotestsrc num-buffers=1 ! identity sleep-time=50000 ! "
        "fakesink sync=false async=false",
        &error);
    assert(error == nullptr);
    assert(pipeline != nullptr);
    return adopt_pipeline(pipeline);
}

ssv::SsvPipelineInstance make_start_failure_pipeline()
{
    GError *error = nullptr;
    GstElement *pipeline = gst_parse_launch(
        "videotestsrc num-buffers=1 ! "
        "fakesink state-error=ready-to-paused",
        &error);
    assert(error == nullptr);
    assert(pipeline != nullptr);
    return adopt_pipeline(pipeline);
}

ssv::SsvPipelineInstance make_display_start_failure_pipeline()
{
    GError *error = nullptr;
    GstElement *pipeline = gst_parse_launch(
        "videotestsrc num-buffers=1 ! "
        "identity name=display-sink-caps ! "
        "fakesink name=display-sink state-error=ready-to-paused",
        &error);
    assert(error == nullptr);
    assert(pipeline != nullptr);
    return adopt_pipeline(pipeline, true);
}

ssv::SsvPipelineInstance make_non_sink_display_start_failure_pipeline()
{
    GError *error = nullptr;
    GstElement *pipeline = gst_parse_launch(
        "videotestsrc num-buffers=1 ! "
        "identity name=display-va-export ! "
        "identity name=display-sink-caps ! "
        "fakesink name=display-sink state-error=ready-to-paused",
        &error);
    assert(error == nullptr);
    assert(pipeline != nullptr);
    return adopt_pipeline(pipeline, true);
}

class RecordingWindow final : public ssv::SsvWindowLifecycle {
public:
    explicit RecordingWindow(std::vector<std::string> &events)
        : events_(events)
    {
    }

    ~RecordingWindow() override { events_.emplace_back("window.destroy"); }

    void show(std::function<void()> on_close) override
    {
        on_close_ = std::move(on_close);
    }

    void close() noexcept override { events_.emplace_back("window.close"); }

private:
    std::vector<std::string> &events_;
    std::function<void()> on_close_;
};

class ClosingWindow final : public ssv::SsvWindowLifecycle {
public:
    explicit ClosingWindow(std::vector<std::string> &events)
        : events_(events)
    {
    }

    ~ClosingWindow() override { events_.emplace_back("window.destroy"); }

    void show(std::function<void()> on_close) override
    {
        events_.emplace_back("window.show");
        g_idle_add_full(
            G_PRIORITY_DEFAULT,
            [](gpointer data) -> gboolean {
                (*static_cast<std::function<void()> *>(data))();
                return G_SOURCE_REMOVE;
            },
            new std::function<void()>(std::move(on_close)),
            [](gpointer data) {
                delete static_cast<std::function<void()> *>(data);
            });
    }

    void close() noexcept override { events_.emplace_back("window.close"); }

private:
    std::vector<std::string> &events_;
};

void assert_pipeline_is_null(GstElement *pipeline)
{
    GstState state = GST_STATE_VOID_PENDING;
    const auto result = gst_element_get_state(
        pipeline, &state, nullptr, GST_SECOND);
    assert(result != GST_STATE_CHANGE_FAILURE);
    assert(state == GST_STATE_NULL);
}

void test_finite_pipeline_reaches_playing_and_eos()
{
    const auto config = make_headless_config();
    auto pipeline = make_finite_pipeline();
    GstElement *observed_pipeline = GST_ELEMENT(
        gst_object_ref(pipeline.pipeline()));
    ssv::SsvRunAttempt runner(
        config, make_headless_plan(config), std::move(pipeline));

    assert(runner.state() == ssv::SsvRunAttemptState::Ready);
    const auto result = runner.run();

    assert(result.exit_code == ssv::SsvExitCode::Success);
    assert(result.stop_reason == ssv::SsvRunAttemptStopReason::EndOfStream);
    assert(result.reached_playing);
    assert(result.error.empty());
    assert(runner.state() == ssv::SsvRunAttemptState::Stopped);
    assert_pipeline_is_null(observed_pipeline);
    gst_object_unref(observed_pipeline);
}

void test_runtime_resolved_waits_for_all_contract_boundaries()
{
    const auto config = make_headless_config();
    const auto plan = make_headless_plan(config);

    std::string complete_output;
    auto complete_log = make_event_log(complete_output);
    auto complete_pipeline = make_finite_pipeline();
    post_contract_ready(
        complete_pipeline.pipeline(), ssv::SsvPipelineBoundary::DecodeTee);
    post_contract_ready(
        complete_pipeline.pipeline(), ssv::SsvPipelineBoundary::DecodeTee);
    ssv::SsvRunAttemptOptions complete_options;
    complete_options.event_log = complete_log.get();
    ssv::SsvRunAttempt complete_runner(
        config,
        plan,
        std::move(complete_pipeline),
        {},
        complete_options);

    assert(complete_runner.run().exit_code == ssv::SsvExitCode::Success);
    assert(count_occurrences(
               complete_output, "event=runtime_resolved ")
        == 1);

    std::string incomplete_output;
    auto incomplete_log = make_event_log(incomplete_output);
    ssv::SsvRunAttemptOptions incomplete_options;
    incomplete_options.event_log = incomplete_log.get();
    ssv::SsvRunAttempt incomplete_runner(
        config,
        plan,
        make_finite_pipeline(),
        {},
        incomplete_options);

    assert(incomplete_runner.run().exit_code == ssv::SsvExitCode::Success);
    assert(incomplete_output.find("event=runtime_resolved ")
        == std::string::npos);
}

void test_fake_bus_error_returns_runtime_failure()
{
    const auto config = make_headless_config();
    auto pipeline = make_live_pipeline();
    GstElement *observed_pipeline = GST_ELEMENT(
        gst_object_ref(pipeline.pipeline()));
    ssv::SsvRunAttempt runner(
        config, make_headless_plan(config), std::move(pipeline));
    std::thread error_injector([&runner, observed_pipeline] {
        while (runner.state() != ssv::SsvRunAttemptState::Playing) {
            assert(runner.state() != ssv::SsvRunAttemptState::Stopped);
            std::this_thread::yield();
        }
        post_fake_bus_error(observed_pipeline);
    });

    const auto result = runner.run();
    error_injector.join();

    assert(result.exit_code == ssv::SsvExitCode::RuntimeFailure);
    assert(result.stop_reason == ssv::SsvRunAttemptStopReason::RuntimeError);
    assert(result.stage == "runtime");
    assert(result.error.empty() == false);
    assert(runner.state() == ssv::SsvRunAttemptState::Stopped);
    assert(!error_injector.joinable());
    assert_pipeline_is_null(observed_pipeline);
    gst_object_unref(observed_pipeline);
}

void test_bus_warning_emits_native_diagnostic_and_continues_to_eos()
{
    const auto config = make_headless_config();
    auto pipeline = make_live_pipeline();
    GstElement *observed_pipeline = GST_ELEMENT(
        gst_object_ref(pipeline.pipeline()));
    std::string output;
    auto event_log = make_event_log(output);
    ssv::SsvRunAttemptOptions options;
    options.event_log = event_log.get();
    options.run_attempt_id = 17;
    ssv::SsvRunAttempt runner(
        config,
        make_headless_plan(config),
        std::move(pipeline),
        {},
        options);
    std::thread warning_injector([&runner, observed_pipeline] {
        while (runner.state() != ssv::SsvRunAttemptState::Playing) {
            assert(runner.state() != ssv::SsvRunAttemptState::Stopped);
            std::this_thread::yield();
        }
        post_fake_bus_warning(observed_pipeline);
        assert(gst_element_send_event(
            observed_pipeline, gst_event_new_eos()));
    });

    const auto result = runner.run();
    warning_injector.join();

    assert(result.exit_code == ssv::SsvExitCode::Success);
    assert(result.stop_reason == ssv::SsvRunAttemptStopReason::EndOfStream);
    assert(output.find(
        "event=native_diagnostic source_id=runner-test run_attempt_id=17 ")
        != std::string::npos);
    assert(output.find("origin=gstreamer source_severity=warning")
        != std::string::npos);
    assert(output.find("domain=ssv-runner-test code=2")
        != std::string::npos);
    assert(output.find("message=\"fake bus warning\"")
        != std::string::npos);
    assert(output.find("debug=\"injected warning debug context\"")
        != std::string::npos);
    assert(runner.state() == ssv::SsvRunAttemptState::Stopped);
    assert_pipeline_is_null(observed_pipeline);
    gst_object_unref(observed_pipeline);
}

void test_display_element_error_is_classified_for_backend_recovery()
{
    auto config = make_headless_config();
    config.display.enabled = true;
    std::vector<std::string> events;
    ssv::SsvRunAttemptOwnedResources resources;
    resources.window = std::make_unique<RecordingWindow>(events);
    auto pipeline = make_display_live_pipeline();
    GstElement *display_element = gst_bin_get_by_name(
        GST_BIN(pipeline.pipeline()), "display-gl-upload");
    assert(display_element != nullptr);
    ssv::SsvRunAttempt runner(
        config,
        make_enabled_plan(config),
        std::move(pipeline),
        std::move(resources));
    std::thread error_injector([&runner, display_element] {
        while (runner.state() != ssv::SsvRunAttemptState::Playing) {
            assert(runner.state() != ssv::SsvRunAttemptState::Stopped);
            std::this_thread::yield();
        }
        post_fake_bus_error(display_element);
    });

    const auto result = runner.run();
    error_injector.join();

    assert(result.exit_code == ssv::SsvExitCode::RuntimeFailure);
    assert(result.stage == "display.runtime");
    gst_object_unref(display_element);
}

void test_state_change_failure_returns_pipeline_contract_failure()
{
    const auto config = make_headless_config();
    ssv::SsvRunAttempt runner(
        config,
        make_headless_plan(config),
        make_start_failure_pipeline());

    const auto result = runner.run();

    assert(result.exit_code == ssv::SsvExitCode::PipelineContractFailed);
    assert(result.stop_reason
        == ssv::SsvRunAttemptStopReason::PipelineStartFailure);
    assert(!result.reached_playing);
    assert(result.stage == "pipeline.start");
    assert(result.error.empty() == false);
    assert(runner.state() == ssv::SsvRunAttemptState::Stopped);
}

void test_display_state_change_failure_keeps_display_recovery_stage()
{
    auto config = make_headless_config();
    config.display.enabled = true;
    const auto plan = make_enabled_plan(config);
    assert(plan.display_fallback_allowed);
    std::vector<std::string> events;
    ssv::SsvRunAttemptOwnedResources resources;
    resources.window = std::make_unique<RecordingWindow>(events);
    ssv::SsvRunAttempt runner(
        config,
        plan,
        make_display_start_failure_pipeline(),
        std::move(resources));

    const auto result = runner.run();

    assert(result.exit_code == ssv::SsvExitCode::PipelineContractFailed);
    assert(result.stop_reason
        == ssv::SsvRunAttemptStopReason::PipelineStartFailure);
    assert(!result.reached_playing);
    assert(result.stage == "display.start");
    assert(result.error.empty() == false);
}

void test_gtk_gl_resource_start_failure_returns_display_initialization_failure()
{
    auto config = make_headless_config();
    config.display.enabled = true;
    std::vector<std::string> events;
    ssv::SsvRunAttemptOwnedResources resources;
    resources.window = std::make_unique<RecordingWindow>(events);
    auto pipeline = make_display_start_failure_pipeline();
    GstElement *display_sink = gst_bin_get_by_name(
        GST_BIN(pipeline.pipeline()), "display-sink");
    assert(display_sink != nullptr);
    post_resource_not_found_bus_error(
        display_sink, "Failed to initialize OpenGL with Gtk");
    ssv::SsvRunAttempt runner(
        config,
        make_enabled_plan(config),
        std::move(pipeline),
        std::move(resources));

    const auto result = runner.run();

    assert(result.exit_code
        == ssv::SsvExitCode::DisplayInitializationFailed);
    assert(result.stop_reason
        == ssv::SsvRunAttemptStopReason::PipelineStartFailure);
    assert(!result.reached_playing);
    assert(result.stage == "display.gl.init");
    assert(result.error.empty() == false);
    gst_object_unref(display_sink);
}

void test_non_sink_display_resource_failure_stays_pipeline_contract_failure()
{
    auto config = make_headless_config();
    config.display.enabled = true;
    std::vector<std::string> events;
    ssv::SsvRunAttemptOwnedResources resources;
    resources.window = std::make_unique<RecordingWindow>(events);
    auto pipeline = make_non_sink_display_start_failure_pipeline();
    GstElement *va_export = gst_bin_get_by_name(
        GST_BIN(pipeline.pipeline()), "display-va-export");
    assert(va_export != nullptr);
    post_resource_not_found_bus_error(
        va_export, "fake VA display resource is unavailable");
    ssv::SsvRunAttempt runner(
        config,
        make_enabled_plan(config),
        std::move(pipeline),
        std::move(resources));

    const auto result = runner.run();

    assert(result.exit_code == ssv::SsvExitCode::PipelineContractFailed);
    assert(result.stop_reason
        == ssv::SsvRunAttemptStopReason::PipelineStartFailure);
    assert(!result.reached_playing);
    assert(result.stage == "display.start");
    gst_object_unref(va_export);
}

void test_contract_error_stays_code_five_after_playing()
{
    const auto config = make_headless_config();
    auto pipeline = make_live_pipeline();
    GstElement *observed_pipeline = GST_ELEMENT(
        gst_object_ref(pipeline.pipeline()));
    std::string output;
    auto event_log = make_event_log(output);
    ssv::SsvRunAttemptOptions options;
    options.event_log = event_log.get();
    ssv::SsvRunAttempt runner(
        config,
        make_headless_plan(config),
        std::move(pipeline),
        {},
        options);
    std::thread error_injector([&runner, observed_pipeline] {
        while (runner.state() != ssv::SsvRunAttemptState::Playing) {
            assert(runner.state() != ssv::SsvRunAttemptState::Stopped);
            std::this_thread::yield();
        }
        post_contract_bus_error(observed_pipeline);
    });

    const auto result = runner.run();
    error_injector.join();

    assert(result.exit_code == ssv::SsvExitCode::PipelineContractFailed);
    assert(result.stop_reason
        == ssv::SsvRunAttemptStopReason::PipelineContractFailure);
    assert(result.reached_playing);
    assert(result.stage == "pipeline.contract");
    assert(output.find("event=buffer_contract_failed ")
        != std::string::npos);
    assert(output.find("stage=pipeline.contract")
        != std::string::npos);
    assert(output.find("expected_allocator=VAMemory")
        != std::string::npos);
    assert(output.find("actual_memory=SystemMemory")
        != std::string::npos);
    assert(output.find("event=runtime_resolved ")
        == std::string::npos);
    gst_object_unref(observed_pipeline);
}

void test_inference_stats_timer_is_removed_before_service_release()
{
    const char *label_map = std::getenv("SSV_TEST_LABEL_MAP_PATH");
    assert(label_map != nullptr && label_map[0] != '\0');
    auto config = make_headless_config();
    config.inference.enabled = true;
    config.inference.model.path = "/bin/true";
    config.inference.model.output_format = "yolo_nx6";
    config.inference.model.label_map = label_map;
    std::vector<std::string> events;
    ssv::SsvRunAttemptOwnedResources resources;
    ssv::infer::SsvInferenceTestServiceOptions service_options;
    service_options.on_backend_destroyed = [&events] {
        events.emplace_back("inference.backend.destroy");
        throw std::runtime_error("test destruction callback");
    };
    resources.service = ssv::infer::ssv_inference_test_service_create(
        config.inference, std::move(service_options));
    std::string output;
    auto event_log = make_event_log(output);
    ssv::SsvRunAttemptOptions options;
    options.event_log = event_log.get();
    options.inference_runtime_snapshot =
        ssv::infer::ssv_inference_service_runtime_snapshot(
            resources.service.get());
    options.inference_stats_interval = 5ms;
    auto pipeline = make_slow_finite_pipeline();
    post_contract_ready(
        pipeline.pipeline(), ssv::SsvPipelineBoundary::DecodeTee);
    post_contract_ready(
        pipeline.pipeline(), ssv::SsvPipelineBoundary::AnalysisGpuInput);
    post_contract_ready(
        pipeline.pipeline(), ssv::SsvPipelineBoundary::AnalysisHost);
    ssv::SsvRunAttempt runner(
        config,
        make_enabled_plan(config),
        std::move(pipeline),
        std::move(resources),
        options);

    const auto result = runner.run();

    assert(result.exit_code == ssv::SsvExitCode::Success);
    assert(output.find("event=inference_stats ")
        != std::string::npos);
    assert(count_occurrences(output, "event=runtime_resolved ") == 1);
    assert(output.find("source_id=runner-test")
        != std::string::npos);
    assert(output.find("input_contract=") != std::string::npos);
    assert(output.find("cache_status=") != std::string::npos);
    assert(output.find("/bin/true") == std::string::npos);
    const auto output_after_run = output;
    std::this_thread::sleep_for(10ms);
    while (g_main_context_iteration(nullptr, FALSE)) {
    }
    assert(output == output_after_run);
    assert(events == std::vector<std::string>({
        "inference.backend.destroy",
    }));
}

void test_runtime_resolved_uses_snapshot_after_service_stop()
{
    const char *label_map = std::getenv("SSV_TEST_LABEL_MAP_PATH");
    assert(label_map != nullptr && label_map[0] != '\0');
    auto config = make_headless_config();
    config.inference.enabled = true;
    config.inference.model.path = "/bin/true";
    config.inference.model.output_format = "yolo_nx6";
    config.inference.model.label_map = label_map;
    std::vector<std::string> events;
    ssv::SsvRunAttemptOwnedResources resources;
    ssv::infer::SsvInferenceTestServiceOptions service_options;
    service_options.on_backend_destroyed = [&events] {
        events.emplace_back("inference.backend.destroy");
    };
    resources.service = ssv::infer::ssv_inference_test_service_create(
        config.inference, std::move(service_options));

    ssv::SsvRunAttemptOptions options;
    options.inference_runtime_snapshot =
        ssv::infer::ssv_inference_service_runtime_snapshot(
            resources.service.get());
    options.inference_runtime_snapshot->provider_chain =
        "CapturedExecutionProvider";
    options.inference_runtime_snapshot->provider_device =
        "captured-device";
    options.inference_runtime_snapshot->precision = "fp16";
    options.inference_runtime_snapshot->model_hash =
        std::string(64, 'f');
    options.inference_runtime_snapshot->cache_status = "hit";
    ssv::infer::ssv_inference_service_stop(resources.service.get());

    std::string output;
    auto event_log = make_event_log(output);
    options.event_log = event_log.get();
    auto pipeline = make_finite_pipeline();
    post_contract_ready(
        pipeline.pipeline(), ssv::SsvPipelineBoundary::DecodeTee);
    post_contract_ready(
        pipeline.pipeline(), ssv::SsvPipelineBoundary::AnalysisGpuInput);
    post_contract_ready(
        pipeline.pipeline(), ssv::SsvPipelineBoundary::AnalysisHost);
    ssv::SsvRunAttempt runner(
        config,
        make_enabled_plan(config),
        std::move(pipeline),
        std::move(resources),
        std::move(options));

    const auto result = runner.run();

    assert(result.exit_code == ssv::SsvExitCode::Success);
    assert(count_occurrences(output, "event=runtime_resolved ") == 1);
    assert(output.find("provider_chain=CapturedExecutionProvider")
        != std::string::npos);
    assert(output.find("provider_device=captured-device")
        != std::string::npos);
    assert(output.find("precision=fp16") != std::string::npos);
    assert(output.find("model_hash=" + std::string(64, 'f'))
        != std::string::npos);
    assert(output.find("input_contract=rgba_u8_nhwc_v1")
        != std::string::npos);
    assert(output.find("cache_status=hit") != std::string::npos);
    assert(events == std::vector<std::string>({
        "inference.backend.destroy",
    }));
}

void test_controlled_signal_returns_success(int signal_number)
{
    const auto config = make_headless_config();
    auto pipeline = make_live_pipeline();
    GstElement *observed_pipeline = GST_ELEMENT(
        gst_object_ref(pipeline.pipeline()));
    ssv::SsvRunAttempt runner(
        config, make_headless_plan(config), std::move(pipeline));
    std::thread signal_sender([&runner, signal_number] {
        while (runner.state() != ssv::SsvRunAttemptState::Playing) {
            assert(runner.state() != ssv::SsvRunAttemptState::Stopped);
            std::this_thread::yield();
        }
        assert(kill(getpid(), signal_number) == 0);
    });

    const auto result = runner.run();
    signal_sender.join();

    assert(result.exit_code == ssv::SsvExitCode::Success);
    assert(result.stop_reason
        == ssv::SsvRunAttemptStopReason::ControlledShutdown);
    assert(result.reached_playing);
    assert(result.error.empty());
    assert(runner.state() == ssv::SsvRunAttemptState::Stopped);
    assert(!signal_sender.joinable());
    assert_pipeline_is_null(observed_pipeline);
    gst_object_unref(observed_pipeline);
}

void test_owned_resources_stop_before_window_destruction()
{
    const char *label_map = std::getenv("SSV_TEST_LABEL_MAP_PATH");
    assert(label_map != nullptr && label_map[0] != '\0');
    auto config = make_headless_config();
    config.display.enabled = true;
    config.inference.enabled = true;
    config.inference.model.path = "/bin/true";
    config.inference.model.output_format = "yolo_nx6";
    config.inference.model.label_map = label_map;
    std::vector<std::string> events;
    ssv::SsvRunAttemptOwnedResources resources;
    ssv::infer::SsvInferenceTestServiceOptions service_options;
    service_options.on_backend_destroyed = [&events] {
        events.emplace_back("inference.backend.destroy");
    };
    resources.service = ssv::infer::ssv_inference_test_service_create(
        config.inference, std::move(service_options));
    ssv::SsvRunAttemptOptions options;
    options.inference_runtime_snapshot =
        ssv::infer::ssv_inference_service_runtime_snapshot(
            resources.service.get());
    ssv::infer::SsvInferenceServicePtr observed_service(
        SSV_INFERENCE_SERVICE(g_object_ref(resources.service.get())));
    resources.window = std::make_unique<RecordingWindow>(events);
    auto pipeline_instance = make_display_finite_pipeline();
    g_object_weak_ref(
        G_OBJECT(pipeline_instance.pipeline()),
        record_pipeline_finalized,
        &events);
    ssv::SsvRunAttempt runner(
        config,
        make_enabled_plan(config),
        std::move(pipeline_instance),
        std::move(resources),
        std::move(options));

    const auto result = runner.run();

    assert(result.exit_code == ssv::SsvExitCode::Success);
    assert(!ssv::infer::ssv_inference_service_is_running(
        observed_service.get()));
    assert(events == std::vector<std::string>({
        "inference.backend.destroy",
        "window.close",
        "window.destroy",
        "pipeline.destroy",
    }));
}

void test_headless_runner_rejects_window_ownership()
{
    const auto config = make_headless_config();
    std::vector<std::string> events;
    ssv::SsvRunAttemptOwnedResources resources;
    resources.window = std::make_unique<RecordingWindow>(events);

    try {
        ssv::SsvRunAttempt runner(
            config,
            make_headless_plan(config),
            make_finite_pipeline(),
            std::move(resources));
        assert(false && "headless runner accepted a window");
    } catch (const std::invalid_argument &) {
    }
}

void test_display_runner_requires_window_ownership()
{
    auto config = make_headless_config();
    config.display.enabled = true;

    try {
        ssv::SsvRunAttempt runner(
            config,
            make_enabled_plan(config),
            make_display_finite_pipeline());
        assert(false && "display runner accepted no window");
    } catch (const std::invalid_argument &error) {
        assert(std::string(error.what()).find("display window")
            != std::string::npos);
    }
}

void test_display_runner_requires_attachment_ownership()
{
    auto config = make_headless_config();
    config.display.enabled = true;
    std::vector<std::string> events;
    ssv::SsvRunAttemptOwnedResources resources;
    resources.window = std::make_unique<RecordingWindow>(events);
    auto pipeline_instance = make_finite_pipeline();
    g_object_weak_ref(
        G_OBJECT(pipeline_instance.pipeline()),
        record_pipeline_finalized,
        &events);

    try {
        ssv::SsvRunAttempt runner(
            config,
            make_enabled_plan(config),
            std::move(pipeline_instance),
            std::move(resources));
        assert(false && "display runner accepted no attachment");
    } catch (const std::invalid_argument &error) {
        assert(std::string(error.what()).find("display attachment")
            != std::string::npos);
    }
    assert(events == std::vector<std::string>({
        "window.destroy",
        "pipeline.destroy",
    }));
}

void test_headless_runner_rejects_attachment_ownership()
{
    const auto config = make_headless_config();
    try {
        ssv::SsvRunAttempt runner(
            config,
            make_headless_plan(config),
            make_display_finite_pipeline());
        assert(false && "headless runner accepted a display attachment");
    } catch (const std::invalid_argument &error) {
        assert(std::string(error.what()).find("display attachment")
            != std::string::npos);
    }
}

void test_window_close_requests_controlled_success()
{
    auto config = make_headless_config();
    config.display.enabled = true;
    std::vector<std::string> events;
    ssv::SsvRunAttemptOwnedResources resources;
    resources.window = std::make_unique<ClosingWindow>(events);
    ssv::SsvRunAttempt runner(
        config,
        make_enabled_plan(config),
        make_display_live_pipeline(),
        std::move(resources));

    const auto result = runner.run();

    assert(result.exit_code == ssv::SsvExitCode::Success);
    assert(result.stop_reason
        == ssv::SsvRunAttemptStopReason::ControlledShutdown);
    assert(events == std::vector<std::string>({
        "window.show",
        "window.close",
        "window.destroy",
    }));
}

void test_run_attempt_requires_matching_inference_runtime_snapshot()
{
    const auto disabled_config = make_headless_config();
    ssv::SsvRunAttemptOptions unexpected_snapshot_options;
    unexpected_snapshot_options.inference_runtime_snapshot.emplace();
    try {
        ssv::SsvRunAttempt runner(
            disabled_config,
            make_headless_plan(disabled_config),
            make_finite_pipeline(),
            {},
            std::move(unexpected_snapshot_options));
        assert(false && "inference-disabled runner accepted a snapshot");
    } catch (const std::invalid_argument &error) {
        assert(std::string_view(error.what()).find("runtime snapshot")
            != std::string_view::npos);
    }

    const char *label_map = std::getenv("SSV_TEST_LABEL_MAP_PATH");
    assert(label_map != nullptr && label_map[0] != '\0');
    auto enabled_config = make_headless_config();
    enabled_config.inference.enabled = true;
    enabled_config.inference.model.path = "/bin/true";
    enabled_config.inference.model.output_format = "yolo_nx6";
    enabled_config.inference.model.label_map = label_map;
    std::vector<std::string> events;
    ssv::SsvRunAttemptOwnedResources resources;
    ssv::infer::SsvInferenceTestServiceOptions service_options;
    service_options.on_backend_destroyed = [&events] {
        events.emplace_back("inference.backend.destroy");
    };
    resources.service = ssv::infer::ssv_inference_test_service_create(
        enabled_config.inference, std::move(service_options));
    try {
        ssv::SsvRunAttempt runner(
            enabled_config,
            make_enabled_plan(enabled_config),
            make_finite_pipeline(),
            std::move(resources));
        assert(false && "inference-enabled runner accepted no snapshot");
    } catch (const std::invalid_argument &error) {
        assert(std::string_view(error.what()).find("runtime snapshot")
            != std::string_view::npos);
    }
}

void test_inference_enabled_runner_requires_service()
{
    auto config = make_headless_config();
    config.inference.enabled = true;

    try {
        ssv::SsvRunAttempt runner(
            config,
            make_enabled_plan(config),
            make_finite_pipeline());
        assert(false && "inference-enabled runner accepted no service");
    } catch (const std::invalid_argument &error) {
        assert(std::string(error.what()).find("inference service")
            != std::string::npos);
    }
}

} // namespace

int main(int argc, char **argv)
{
    gst_init(&argc, &argv);
    test_finite_pipeline_reaches_playing_and_eos();
    test_runtime_resolved_waits_for_all_contract_boundaries();
    test_fake_bus_error_returns_runtime_failure();
    test_bus_warning_emits_native_diagnostic_and_continues_to_eos();
    test_display_element_error_is_classified_for_backend_recovery();
    test_state_change_failure_returns_pipeline_contract_failure();
    test_display_state_change_failure_keeps_display_recovery_stage();
    test_gtk_gl_resource_start_failure_returns_display_initialization_failure();
    test_non_sink_display_resource_failure_stays_pipeline_contract_failure();
    test_contract_error_stays_code_five_after_playing();
    test_inference_stats_timer_is_removed_before_service_release();
    test_runtime_resolved_uses_snapshot_after_service_stop();
    test_controlled_signal_returns_success(SIGINT);
    test_controlled_signal_returns_success(SIGTERM);
    test_owned_resources_stop_before_window_destruction();
    test_headless_runner_rejects_window_ownership();
    test_display_runner_requires_window_ownership();
    test_display_runner_requires_attachment_ownership();
    test_headless_runner_rejects_attachment_ownership();
    test_window_close_requests_controlled_success();
    test_run_attempt_requires_matching_inference_runtime_snapshot();
    test_inference_enabled_runner_requires_service();
    return 0;
}
