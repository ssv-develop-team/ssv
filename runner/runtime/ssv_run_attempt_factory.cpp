#include "ssv_run_attempt_factory.hpp"

#include "display/ssv_display_window.hpp"
#include "observability/ssv_event_log.hpp"
#include "pipeline/ssv_hardware_capabilities.hpp"
#include "pipeline/ssv_pipeline_builder.hpp"
#include "ssv_inference_service.hpp"
#include "ssv_runtime_event_adapter.hpp"

#include <gst/gst.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace ssv {
namespace {

class SsvSystemRunAttemptFactory final : public SsvRunAttemptFactory {
public:
    explicit SsvSystemRunAttemptFactory(SsvEventLog &event_log) noexcept
        : event_log_(event_log)
    {
    }

    SsvHardwareCapabilities prepare_run(
        const SsvConfig &original_config) override
    {
        GError *gst_error = nullptr;
        if (!gst_init_check(nullptr, nullptr, &gst_error)) {
            const std::string message = gst_error != nullptr
                ? gst_error->message
                : "GStreamer initialization failed";
            g_clear_error(&gst_error);
            throw SsvRunAttemptFactoryError(
                SsvExitCode::CapabilityUnavailable,
                "capability.gstreamer",
                message);
        }

        const SsvSystemHardwareCapabilitiesProbe probe;
        capabilities_ = probe.detect();
        if (original_config.display.enabled)
            SsvDisplayWindow::initialize(
                original_config.display.gl_backend);
        return capabilities_;
    }

    SsvRunAttemptCreation create(
        const SsvConfig &effective_config,
        const SsvPipelinePlan &plan,
        SsvEventContext context) override
    {
        SsvRunAttemptOwnedResources resources;
        std::optional<infer::SsvInferenceRuntimeSnapshot>
            inference_runtime_snapshot;
        std::vector<SsvEvent> events;
        if (effective_config.inference.enabled) {
            resources.service = infer::ssv_inference_service_create(
                effective_config.inference);
            inference_runtime_snapshot =
                infer::ssv_inference_service_runtime_snapshot(
                    resources.service.get());
            events = ssv_provider_fallback_events(
                context, *inference_runtime_snapshot);
        }

        auto pipeline_instance = SsvPipelineBuilder::build(
            effective_config,
            plan,
            capabilities_,
            resources.service.get());
        if (effective_config.display.enabled) {
            if (!plan.display_backend) {
                throw SsvDisplayWindowError(
                    "display.gtk.window",
                    "display plan has no resolved backend");
            }
            resources.window = SsvDisplayWindow::create(
                {
                    .source_id = plan.source_id,
                    .gl_backend = effective_config.display.gl_backend,
                    .overlay = effective_config.display.overlay,
                    .backend = *plan.display_backend,
                    .source_context = pipeline_instance.source_context(),
                },
                pipeline_instance.display_attachment());
        }

        SsvRunAttemptOptions options;
        options.event_log = &event_log_;
        options.run_attempt_id = context.run_attempt_id;
        options.inference_runtime_snapshot =
            std::move(inference_runtime_snapshot);
        return {
            .attempt = std::make_unique<SsvRunAttempt>(
                effective_config,
                plan,
                std::move(pipeline_instance),
                std::move(resources),
                std::move(options)),
            .events = std::move(events),
        };
    }

private:
    SsvEventLog &event_log_;
    SsvHardwareCapabilities capabilities_;
};

} // namespace

SsvRunAttemptFactoryError::SsvRunAttemptFactoryError(
    SsvExitCode exit_code,
    std::string stage,
    std::string message)
    : std::runtime_error(std::move(message))
    , exit_code_(exit_code)
    , stage_(std::move(stage))
{
}

SsvExitCode SsvRunAttemptFactoryError::exit_code() const noexcept
{
    return exit_code_;
}

const std::string &SsvRunAttemptFactoryError::stage() const noexcept
{
    return stage_;
}

std::unique_ptr<SsvRunAttemptFactory>
ssv_system_run_attempt_factory(SsvEventLog &event_log)
{
    return std::make_unique<SsvSystemRunAttemptFactory>(event_log);
}

} // namespace ssv
