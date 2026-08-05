#include "ssv_run_attempt.hpp"
#include "observability/ssv_event_log.hpp"
#include "pipeline/ssv_pipeline_contract.hpp"
#include "ssv_runtime_event_adapter.hpp"

#include <glib-unix.h>
#include <gst/gl/gstglcontext.h>
#include <gst/gl/gstglwindow.h>

#include <atomic>
#include <csignal>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace ssv {
namespace {

struct MainContextDeleter {
    void operator()(GMainContext *context) const noexcept
    {
        if (context != nullptr)
            g_main_context_unref(context);
    }
};

struct MainLoopDeleter {
    void operator()(GMainLoop *loop) const noexcept
    {
        if (loop != nullptr)
            g_main_loop_unref(loop);
    }
};

struct SourceDeleter {
    void operator()(GSource *source) const noexcept
    {
        if (source == nullptr)
            return;
        g_source_destroy(source);
        g_source_unref(source);
    }
};

using MainContextPtr =
    std::unique_ptr<GMainContext, MainContextDeleter>;
using MainLoopPtr = std::unique_ptr<GMainLoop, MainLoopDeleter>;
using SourcePtr = std::unique_ptr<GSource, SourceDeleter>;

SourcePtr make_signal_source(
    int signal_number,
    GSourceFunc callback,
    gpointer user_data,
    GMainContext *context)
{
    SourcePtr source(g_unix_signal_source_new(signal_number));
    if (!source)
        throw std::runtime_error("failed to create Unix signal source");
    g_source_set_callback(source.get(), callback, user_data, nullptr);
    if (g_source_attach(source.get(), context) == 0)
        throw std::runtime_error("failed to attach Unix signal source");
    return source;
}

bool is_display_initialization_failure(
    SsvPipelineMessageOrigin message_origin,
    const GError *error,
    const std::optional<SsvResolvedDisplayBackend> &display_backend)
{
    return message_origin != SsvPipelineMessageOrigin::Other
        && error != nullptr
        && (error->domain == GST_GL_CONTEXT_ERROR
            || error->domain == GST_GL_WINDOW_ERROR
            // gtkglsink wraps winsys/context setup failures on the sink.
            || (display_backend == SsvResolvedDisplayBackend::GtkGlSink
                && message_origin == SsvPipelineMessageOrigin::DisplaySink
                && error->domain == GST_RESOURCE_ERROR
                && error->code == GST_RESOURCE_ERROR_NOT_FOUND));
}

std::set<SsvPipelineBoundary> required_contract_boundaries(
    const SsvPipelinePlan &plan)
{
    std::set<SsvPipelineBoundary> boundaries {
        SsvPipelineBoundary::DecodeTee,
    };
    if (plan.expected_caps.display_upload_input)
        boundaries.insert(SsvPipelineBoundary::DisplayUpload);
    if (plan.expected_caps.display_sink_input)
        boundaries.insert(SsvPipelineBoundary::DisplaySink);
    if (plan.expected_caps.analysis_gpu_input)
        boundaries.insert(SsvPipelineBoundary::AnalysisGpuInput);
    if (plan.expected_caps.analysis_host_input)
        boundaries.insert(SsvPipelineBoundary::AnalysisHost);
    return boundaries;
}

} // namespace

class SsvRunAttempt::Impl {
public:
    Impl(
        SsvConfig config,
        SsvPipelinePlan plan,
        SsvPipelineInstance pipeline_instance,
        SsvRunAttemptOwnedResources resources,
        SsvRunAttemptOptions options)
        : config_(std::move(config))
        , plan_(std::move(plan))
        , pipeline_instance_(std::move(pipeline_instance))
        , window_(std::move(resources.window))
        , service_(std::move(resources.service))
        , inference_runtime_snapshot_(
              std::move(options.inference_runtime_snapshot))
        , event_log_(options.event_log)
        , run_attempt_id_(options.run_attempt_id)
        , inference_stats_interval_(options.inference_stats_interval)
        , pending_contract_boundaries_(
              required_contract_boundaries(plan_))
    {
        if (!pipeline_instance_) {
            throw std::invalid_argument(
                "SsvRunAttempt requires a pipeline instance");
        }
        if (config_.sources.size() != 1
            || config_.sources.front().id != plan_.source_id) {
            throw std::invalid_argument(
                "SsvRunAttempt config and pipeline plan source must match");
        }
        if (!plan_.display_backend && window_ != nullptr) {
            throw std::invalid_argument(
                "headless SsvRunAttempt must not own a display window");
        }
        if (plan_.display_backend && window_ == nullptr) {
            throw std::invalid_argument(
                "display-enabled SsvRunAttempt requires a display window");
        }
        if (!plan_.display_backend
            && pipeline_instance_.display_attachment() != nullptr) {
            throw std::invalid_argument(
                "headless SsvRunAttempt must not own a display attachment");
        }
        if (plan_.display_backend
            && pipeline_instance_.display_attachment() == nullptr) {
            throw std::invalid_argument(
                "display-enabled SsvRunAttempt requires a display attachment");
        }
        if (!plan_.inference_backend && service_ != nullptr) {
            throw std::invalid_argument(
                "inference-disabled SsvRunAttempt must not own a service");
        }
        if (plan_.inference_backend && service_ == nullptr) {
            throw std::invalid_argument(
                "inference-enabled SsvRunAttempt requires an inference service");
        }
        if (config_.inference.enabled
            != inference_runtime_snapshot_.has_value()) {
            throw std::invalid_argument(
                "SsvRunAttempt requires inference configuration and runtime snapshot presence to match");
        }
        if (inference_stats_interval_.count() <= 0
            || static_cast<std::uint64_t>(
                   inference_stats_interval_.count())
                > G_MAXUINT) {
            throw std::invalid_argument(
                "SsvRunAttempt inference stats interval is out of range");
        }
    }

    ~Impl()
    {
        stop_runtime_resources();
        detach_sources();
        pipeline_instance_.reset();
        loop_.reset();
        context_.reset();
    }

    SsvRunAttemptResult run()
    {
        if (state_.load() != SsvRunAttemptState::Ready) {
            throw std::logic_error("SsvRunAttempt may only run once");
        }

        create_main_context();
        attach_bus_source();
        attach_inference_stats_source();
        sigint_source_ = make_signal_source(
            SIGINT, &Impl::on_controlled_signal, this, context_.get());
        sigterm_source_ = make_signal_source(
            SIGTERM, &Impl::on_controlled_signal, this, context_.get());
        if (window_ != nullptr) {
            window_->show([this] { request_shutdown(); });
        }

        state_.store(SsvRunAttemptState::Starting);
        if (gst_element_set_state(
                pipeline_instance_.pipeline(), GST_STATE_PLAYING)
            == GST_STATE_CHANGE_FAILURE) {
            if (!handle_synchronous_start_error()) {
                finish({
                    SsvExitCode::PipelineContractFailed,
                    SsvRunAttemptStopReason::PipelineStartFailure,
                    false,
                    "pipeline.start",
                    "GStreamer pipeline failed to enter PLAYING",
                });
            }
            cleanup_after_run();
            return result_;
        }

        g_main_loop_run(loop_.get());
        if (!finished_) {
            finish({
                SsvExitCode::Success,
                SsvRunAttemptStopReason::ControlledShutdown,
                reached_playing_,
                {},
                {},
            });
        }
        cleanup_after_run();
        return result_;
    }

    void request_shutdown() noexcept
    {
        if (finished_)
            return;
        finish({
            SsvExitCode::Success,
            SsvRunAttemptStopReason::ControlledShutdown,
            reached_playing_,
            {},
            {},
        });
    }

    SsvRunAttemptState state() const noexcept { return state_.load(); }

private:
    void create_main_context()
    {
        context_.reset(g_main_context_ref(g_main_context_default()));
        if (!context_)
            throw std::runtime_error("failed to create GLib main context");
        loop_.reset(g_main_loop_new(context_.get(), FALSE));
        if (!loop_)
            throw std::runtime_error("failed to create GLib main loop");
    }

    void attach_bus_source()
    {
        GstBus *bus = gst_element_get_bus(pipeline_instance_.pipeline());
        if (bus == nullptr)
            throw std::runtime_error("pipeline has no GStreamer bus");
        bus_source_.reset(gst_bus_create_watch(bus));
        gst_object_unref(bus);
        if (!bus_source_)
            throw std::runtime_error("failed to create GStreamer bus source");
        g_source_set_callback(
            bus_source_.get(), G_SOURCE_FUNC(&Impl::on_bus_message), this, nullptr);
        if (g_source_attach(bus_source_.get(), context_.get()) == 0)
            throw std::runtime_error("failed to attach GStreamer bus source");
    }

    void attach_inference_stats_source()
    {
        if (service_ == nullptr || event_log_ == nullptr)
            return;
        stats_source_.reset(g_timeout_source_new(
            static_cast<guint>(inference_stats_interval_.count())));
        if (!stats_source_) {
            throw std::runtime_error(
                "failed to create inference stats source");
        }
        g_source_set_callback(
            stats_source_.get(), &Impl::on_inference_stats, this, nullptr);
        if (g_source_attach(stats_source_.get(), context_.get()) == 0) {
            throw std::runtime_error(
                "failed to attach inference stats source");
        }
    }

    static gboolean on_bus_message(
        GstBus *,
        GstMessage *message,
        gpointer user_data)
    {
        auto &self = *static_cast<Impl *>(user_data);
        try {
            switch (GST_MESSAGE_TYPE(message)) {
            case GST_MESSAGE_STATE_CHANGED:
                self.handle_state_changed(message);
                return G_SOURCE_CONTINUE;
            case GST_MESSAGE_ELEMENT:
                self.handle_element(message);
                return G_SOURCE_CONTINUE;
            case GST_MESSAGE_WARNING:
                self.handle_warning(message);
                return G_SOURCE_CONTINUE;
            case GST_MESSAGE_EOS:
                self.finish({
                    SsvExitCode::Success,
                    SsvRunAttemptStopReason::EndOfStream,
                    self.reached_playing_,
                    {},
                    {},
                });
                return G_SOURCE_REMOVE;
            case GST_MESSAGE_ERROR:
                self.handle_error(message);
                return G_SOURCE_REMOVE;
            default:
                return G_SOURCE_CONTINUE;
            }
        } catch (const std::exception &error) {
            self.finish({
                SsvExitCode::RuntimeFailure,
                SsvRunAttemptStopReason::RuntimeError,
                self.reached_playing_,
                "runtime.observability",
                error.what(),
            });
            return G_SOURCE_REMOVE;
        } catch (...) {
            self.finish({
                SsvExitCode::RuntimeFailure,
                SsvRunAttemptStopReason::RuntimeError,
                self.reached_playing_,
                "runtime.observability",
                "unknown runtime observability failure",
            });
            return G_SOURCE_REMOVE;
        }
    }

    static gboolean on_inference_stats(gpointer user_data)
    {
        auto &self = *static_cast<Impl *>(user_data);
        try {
            const auto stats =
                infer::ssv_inference_service_take_stats_window(
                    self.service_.get());
            self.event_log_->emit(ssv_inference_stats_event(
                self.event_context(), stats));
            return G_SOURCE_CONTINUE;
        } catch (const std::exception &error) {
            self.finish({
                SsvExitCode::RuntimeFailure,
                SsvRunAttemptStopReason::RuntimeError,
                self.reached_playing_,
                "inference.stats",
                error.what(),
            });
            return G_SOURCE_REMOVE;
        } catch (...) {
            self.finish({
                SsvExitCode::RuntimeFailure,
                SsvRunAttemptStopReason::RuntimeError,
                self.reached_playing_,
                "inference.stats",
                "unknown inference stats failure",
            });
            return G_SOURCE_REMOVE;
        }
    }

    void handle_element(GstMessage *message)
    {
        const auto ready =
            ssv_pipeline_contract_ready_from_message(message);
        if (!ready)
            return;
        pending_contract_boundaries_.erase(*ready);
        if (pending_contract_boundaries_.empty()
            && !runtime_resolved_emitted_
            && event_log_ != nullptr) {
            runtime_resolved_emitted_ = true;
            event_log_->emit(ssv_runtime_resolved_event(
                event_context(),
                config_,
                plan_,
                inference_runtime_snapshot_));
        }
    }

    void handle_state_changed(GstMessage *message)
    {
        if (GST_MESSAGE_SRC(message)
            != GST_OBJECT(pipeline_instance_.pipeline()))
            return;
        GstState old_state = GST_STATE_VOID_PENDING;
        GstState new_state = GST_STATE_VOID_PENDING;
        GstState pending_state = GST_STATE_VOID_PENDING;
        gst_message_parse_state_changed(
            message, &old_state, &new_state, &pending_state);
        if (new_state == GST_STATE_PLAYING) {
            reached_playing_ = true;
            state_.store(SsvRunAttemptState::Playing);
        }
    }

    bool handle_synchronous_start_error()
    {
        GstBus *bus = gst_element_get_bus(pipeline_instance_.pipeline());
        if (bus == nullptr)
            return false;
        GstMessage *message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
        gst_object_unref(bus);
        if (message == nullptr)
            return false;
        handle_error(message);
        gst_message_unref(message);
        return true;
    }

    void handle_error(GstMessage *message)
    {
        const auto contract_violation =
            ssv_pipeline_contract_violation_from_message(message);
        GError *gerror = nullptr;
        gchar *debug = nullptr;
        gst_message_parse_error(message, &gerror, &debug);
        const std::string error = gerror != nullptr
            ? gerror->message
            : "unknown GStreamer error";
        const bool contract_failure = gerror != nullptr
            && gerror->domain == ssv_pipeline_contract_error_quark();
        const auto message_origin =
            pipeline_instance_.message_origin(message);
        const bool display_failure = message_origin
            != SsvPipelineMessageOrigin::Other;
        const bool gl_window_or_context_failure =
            is_display_initialization_failure(
                message_origin, gerror, plan_.display_backend);
        g_clear_error(&gerror);
        g_free(debug);

        GstState current_state = GST_STATE_VOID_PENDING;
        gst_element_get_state(
            pipeline_instance_.pipeline(), &current_state, nullptr, 0);
        const bool failed_during_runtime = reached_playing_
            || current_state == GST_STATE_PLAYING;
        reached_playing_ = failed_during_runtime;
        const bool display_initialization_failure = display_failure
            && !failed_during_runtime
            && gl_window_or_context_failure;
        SsvExitCode exit_code = SsvExitCode::PipelineContractFailed;
        SsvRunAttemptStopReason stop_reason =
            SsvRunAttemptStopReason::PipelineStartFailure;
        std::string stage = display_failure
            ? "display.start"
            : "pipeline.start";
        if (contract_failure) {
            stop_reason = SsvRunAttemptStopReason::PipelineContractFailure;
            stage = display_failure
                ? "display.contract"
                : "pipeline.contract";
        } else if (failed_during_runtime) {
            exit_code = SsvExitCode::RuntimeFailure;
            stop_reason = SsvRunAttemptStopReason::RuntimeError;
            stage = display_failure ? "display.runtime" : "runtime";
        } else if (display_initialization_failure) {
            exit_code = SsvExitCode::DisplayInitializationFailed;
            stage = "display.gl.init";
        }
        if (contract_failure && contract_violation
            && event_log_ != nullptr) {
            event_log_->emit(ssv_buffer_contract_failed_event(
                event_context(), stage, *contract_violation));
        }
        finish({
            exit_code,
            stop_reason,
            reached_playing_,
            std::move(stage),
            error,
        });
    }

    void handle_warning(GstMessage *message)
    {
        if (event_log_ == nullptr)
            return;
        event_log_->emit(ssv_gstreamer_warning_event(
            event_context(), message));
    }

    static gboolean on_controlled_signal(gpointer user_data)
    {
        auto &self = *static_cast<Impl *>(user_data);
        self.finish({
            SsvExitCode::Success,
            SsvRunAttemptStopReason::ControlledShutdown,
            self.reached_playing_,
            {},
            {},
        });
        return G_SOURCE_REMOVE;
    }

    void finish(SsvRunAttemptResult result) noexcept
    {
        if (finished_)
            return;
        finished_ = true;
        result_ = std::move(result);
        stop_runtime_resources();
        state_.store(SsvRunAttemptState::Stopped);
        if (loop_ != nullptr)
            g_main_loop_quit(loop_.get());
    }

    void stop_runtime_resources() noexcept
    {
        if (runtime_resources_stopped_)
            return;
        runtime_resources_stopped_ = true;
        stats_source_.reset();
        if (pipeline_instance_)
            gst_element_set_state(
                pipeline_instance_.pipeline(), GST_STATE_NULL);
        if (service_ != nullptr) {
            infer::ssv_inference_service_stop(service_.get());
            service_.reset();
        }
        if (window_ != nullptr) {
            window_->close();
            window_.reset();
        }
    }

    void detach_sources() noexcept
    {
        sigterm_source_.reset();
        sigint_source_.reset();
        bus_source_.reset();
    }

    void cleanup_after_run() noexcept
    {
        detach_sources();
        pipeline_instance_.reset();
        loop_.reset();
        context_.reset();
    }

    [[nodiscard]] SsvEventContext event_context() const
    {
        return {
            .source_id = plan_.source_id,
            .run_attempt_id = run_attempt_id_,
        };
    }

    SsvConfig config_;
    SsvPipelinePlan plan_;
    SsvPipelineInstance pipeline_instance_;
    std::unique_ptr<SsvWindowLifecycle> window_;
    infer::SsvInferenceServicePtr service_;
    std::optional<infer::SsvInferenceRuntimeSnapshot>
        inference_runtime_snapshot_;
    SsvEventLog *event_log_;
    std::optional<std::uint32_t> run_attempt_id_;
    std::chrono::milliseconds inference_stats_interval_;
    std::set<SsvPipelineBoundary> pending_contract_boundaries_;
    MainContextPtr context_;
    MainLoopPtr loop_;
    SourcePtr bus_source_;
    SourcePtr sigint_source_;
    SourcePtr sigterm_source_;
    SourcePtr stats_source_;
    std::atomic<SsvRunAttemptState> state_ {SsvRunAttemptState::Ready};
    SsvRunAttemptResult result_ {
        SsvExitCode::Success,
        SsvRunAttemptStopReason::ControlledShutdown,
        false,
        {},
        {},
    };
    bool reached_playing_ = false;
    bool runtime_resolved_emitted_ = false;
    bool finished_ = false;
    bool runtime_resources_stopped_ = false;
};

SsvRunAttempt::SsvRunAttempt() noexcept = default;

SsvRunAttempt::SsvRunAttempt(
    SsvConfig config,
    SsvPipelinePlan plan,
    SsvPipelineInstance pipeline_instance,
    SsvRunAttemptOwnedResources resources,
    SsvRunAttemptOptions options)
    : impl_(std::make_unique<Impl>(
        std::move(config),
        std::move(plan),
        std::move(pipeline_instance),
        std::move(resources),
        std::move(options)))
{
}

SsvRunAttempt::~SsvRunAttempt() = default;

SsvRunAttemptResult SsvRunAttempt::run()
{
    return impl_->run();
}

void SsvRunAttempt::request_shutdown() noexcept
{
    impl_->request_shutdown();
}

SsvRunAttemptState SsvRunAttempt::state() const noexcept
{
    return impl_->state();
}

} // namespace ssv
