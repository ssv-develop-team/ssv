#include "ssv_runtime_event_adapter.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace ssv {
namespace {

std::string_view memory_name(SsvMemoryKind memory) noexcept
{
    switch (memory) {
    case SsvMemoryKind::SystemMemory: return "SystemMemory";
    case SsvMemoryKind::VaMemory: return "VAMemory";
    case SsvMemoryKind::DmaBuf: return "DMABuf";
    case SsvMemoryKind::GlMemory: return "GLMemory";
    case SsvMemoryKind::Unknown: return "unknown";
    }
    return "unknown";
}

std::string_view display_name(
    std::optional<SsvResolvedDisplayBackend> backend) noexcept
{
    if (!backend)
        return "disabled";
    return *backend == SsvResolvedDisplayBackend::GtkGlSink
        ? "gtkglsink"
        : "gtksink";
}

} // namespace

SsvEvent ssv_runtime_resolved_event(
    SsvEventContext context,
    const SsvConfig &config,
    const SsvPipelinePlan &plan,
    const std::optional<infer::SsvInferenceRuntimeSnapshot>
        &inference_runtime_snapshot)
{
    if (config.sources.size() != 1
        || config.sources.front().id != plan.source_id
        || context.source_id != plan.source_id) {
        throw std::invalid_argument(
            "runtime_resolved requires matching single-source context, config, and plan");
    }
    if (config.inference.enabled
        != inference_runtime_snapshot.has_value()) {
        throw std::invalid_argument(
            "runtime_resolved requires inference configuration and runtime snapshot presence to match");
    }
    const auto *inference = inference_runtime_snapshot
        ? &*inference_runtime_snapshot
        : nullptr;
    const bool vaapi = plan.decode.backend == SsvDecodeBackend::Vaapi;
    const bool gtk_gl = plan.display_backend
        == SsvResolvedDisplayBackend::GtkGlSink;
    return {
        .context = std::move(context),
        .payload = SsvRuntimeResolvedEvent {
            .decoder = plan.decode.decoder_factory,
            .va_device = vaapi
                ? plan.decode.device.value
                : "not-applicable",
            .va_driver = vaapi ? "unknown" : "not-applicable",
            .decode_memory = std::string(memory_name(
                plan.expected_caps.decode_output.memory)),
            .vpp = plan.decode.va_postproc_factory.empty()
                ? "disabled"
                : plan.decode.va_postproc_factory,
            .display_backend = std::string(display_name(
                plan.display_backend)),
            .egl_renderer = gtk_gl ? "unknown" : "not-applicable",
            .provider_chain = inference != nullptr
                ? inference->provider_chain
                : "disabled",
            .provider_device = inference != nullptr
                ? inference->provider_device
                : "not-applicable",
            .precision = inference != nullptr
                ? inference->precision
                : "not-applicable",
            .model_hash = inference != nullptr
                ? inference->model_hash
                : "not-applicable",
            .input_contract = inference != nullptr
                ? inference->input_contract
                : "not-applicable",
            .cache_status = inference != nullptr
                ? inference->cache_status
                : "disabled",
        },
    };
}

std::vector<SsvEvent> ssv_provider_fallback_events(
    const SsvEventContext &context,
    const infer::SsvInferenceRuntimeSnapshot &snapshot)
{
    std::vector<SsvEvent> events;
    events.reserve(snapshot.fallbacks.size());
    for (const auto &fallback : snapshot.fallbacks) {
        events.push_back({
            .context = context,
            .payload = SsvAccelerationFallbackEvent {
                .from = fallback.provider,
                .to = fallback.resolved_provider_chain,
                .stage = "inference.provider." + fallback.stage,
                .reason = fallback.reason,
            },
        });
    }
    return events;
}

SsvEvent ssv_buffer_contract_failed_event(
    SsvEventContext context,
    std::string stage,
    const SsvPipelineContractViolation &violation)
{
    return {
        .context = std::move(context),
        .payload = SsvBufferContractFailedEvent {
            .stage = std::move(stage),
            .expected_caps = violation.expected_caps,
            .expected_allocator = violation.expected_allocator,
            .expected_memory = violation.expected_memory,
            .actual_caps = violation.actual_caps,
            .actual_allocator = violation.actual_allocator,
            .actual_memory = violation.actual_memory,
            .reason = violation.message,
        },
    };
}

SsvEvent ssv_inference_stats_event(
    SsvEventContext context,
    const infer::SsvInferenceStatsWindow &stats)
{
    const auto duration = [](std::uint64_t microseconds) {
        using Rep = std::chrono::microseconds::rep;
        const auto maximum = static_cast<std::uint64_t>(
            std::numeric_limits<Rep>::max());
        return std::chrono::microseconds {
            static_cast<Rep>(std::min(microseconds, maximum)),
        };
    };
    const auto percentiles = [&](const auto &source) {
        return SsvLatencyPercentiles {
            .p50 = source.p50_us
                ? std::optional {duration(*source.p50_us)}
                : std::nullopt,
            .p95 = source.p95_us
                ? std::optional {duration(*source.p95_us)}
                : std::nullopt,
        };
    };
    return {
        .context = std::move(context),
        .payload = SsvInferenceStatsEvent {
            .received = stats.received,
            .dropped = stats.dropped,
            .completed = stats.completed,
            .completed_fps = stats.completed_fps,
            .longest_result_gap = duration(stats.longest_result_gap_us),
            .queue = percentiles(stats.queue),
            .decode = percentiles(stats.decode),
            .color_resize = percentiles(stats.color_resize),
            .normalize_layout = percentiles(stats.normalize_layout),
            .backend_h2d = percentiles(stats.backend_h2d),
            .backend_execution = percentiles(stats.backend_execution),
            .backend_d2h = percentiles(stats.backend_d2h),
            .backend_unattributed = percentiles(stats.backend_unattributed),
            .postprocess = percentiles(stats.postprocess),
            .total = percentiles(stats.total),
        },
    };
}

SsvEvent ssv_gstreamer_warning_event(
    SsvEventContext context,
    GstMessage *message)
{
    if (message == nullptr
        || GST_MESSAGE_TYPE(message) != GST_MESSAGE_WARNING) {
        throw std::invalid_argument(
            "native diagnostic adapter requires a GStreamer warning");
    }

    GError *error = nullptr;
    gchar *debug = nullptr;
    gst_message_parse_warning(message, &error, &debug);
    const char *domain_name = error != nullptr
        ? g_quark_to_string(error->domain)
        : nullptr;
    std::string domain = domain_name != nullptr ? domain_name : "unknown";
    const int code = error != nullptr ? error->code : 0;
    std::string warning_message = error != nullptr
        ? error->message
        : "unknown GStreamer warning";
    std::optional<std::string> debug_snapshot;
    if (debug != nullptr && *debug != '\0')
        debug_snapshot = debug;
    g_clear_error(&error);
    g_free(debug);

    return {
        .context = std::move(context),
        .payload = SsvNativeDiagnosticEvent {
            .origin = "gstreamer",
            .source_severity = SsvNativeDiagnosticSeverity::Warning,
            .domain = std::move(domain),
            .code = code,
            .message = std::move(warning_message),
            .debug = std::move(debug_snapshot),
        },
    };
}

} // namespace ssv
