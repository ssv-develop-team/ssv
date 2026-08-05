#include "ssv_run_fallback_state.hpp"

#include "pipeline/ssv_pipeline_contract.hpp"

#include <string>
#include <utility>
#include <variant>

namespace ssv {
namespace {

std::string_view decode_backend_name(SsvDecodeBackend backend) noexcept
{
    switch (backend) {
    case SsvDecodeBackend::Vaapi:
        return "vaapi";
    case SsvDecodeBackend::Nvdec:
        return "nvdec";
    case SsvDecodeBackend::Software:
        return "software";
    }
    return "unknown";
}

std::string join_reasons(const std::vector<std::string> &reasons)
{
    std::string result;
    for (const auto &reason : reasons) {
        if (!result.empty())
            result += "; ";
        result += reason;
    }
    return result;
}

} // namespace

SsvConfig SsvRunFallbackState::derive_effective_config(
    const SsvConfig &original_config) const
{
    auto effective_config = original_config;
    if (force_gtk_sink_)
        effective_config.display.backend = SsvDisplayBackend::GtkSink;
    if (force_software_decode_) {
        effective_config.sources.front().decode.mode = SsvDecodeMode::Software;
        effective_config.sources.front().decode.device = {};
    }
    return effective_config;
}

std::optional<SsvEvent> SsvRunFallbackState::take_creation_event(
    SsvEvent event)
{
    const auto *fallback = std::get_if<SsvAccelerationFallbackEvent>(
        &event.payload);
    if (fallback == nullptr)
        return event;

    auto key = std::make_tuple(
        event.context.source_id,
        fallback->from,
        fallback->to,
        fallback->stage,
        fallback->reason);
    if (!emitted_fallbacks_.insert(std::move(key)).second)
        return std::nullopt;
    return event;
}

std::vector<SsvEvent> SsvRunFallbackState::take_plan_events(
    const SsvPipelinePlan &plan,
    const SsvEventContext &attempt_context)
{
    std::vector<SsvEvent> events;
    events.reserve(
        plan.decode_fallbacks.size()
        + (plan.display_fallback_reasons.empty() ? 0U : 1U));
    for (const auto &fallback : plan.decode_fallbacks) {
        auto event = take_creation_event({
            .context = attempt_context,
            .payload = SsvAccelerationFallbackEvent {
                .from = std::string(decode_backend_name(fallback.from)),
                .to = std::string(decode_backend_name(fallback.to)),
                .stage = "decode.resolve",
                .reason = fallback.reason,
            },
        });
        if (event)
            events.push_back(std::move(*event));
    }
    if (!plan.display_fallback_reasons.empty()) {
        display_fallback_attempted_ = true;
        auto event = take_creation_event({
            .context = attempt_context,
            .payload = SsvAccelerationFallbackEvent {
                .from = "gtkglsink",
                .to = "gtksink",
                .stage = "display.resolve",
                .reason = join_reasons(plan.display_fallback_reasons),
            },
        });
        if (event)
            events.push_back(std::move(*event));
    }
    return events;
}

std::optional<SsvEvent> SsvRunFallbackState::try_display_fallback(
    const SsvPipelinePlan &plan,
    SsvEventContext failed_attempt,
    std::string_view stage,
    std::string_view reason)
{
    if (display_fallback_attempted_
        || !plan.display_fallback_allowed
        || !stage.starts_with("display.")) {
        return std::nullopt;
    }

    display_fallback_attempted_ = true;
    force_gtk_sink_ = true;
    return SsvEvent {
        .context = std::move(failed_attempt),
        .payload = SsvAccelerationFallbackEvent {
            .from = "gtkglsink",
            .to = "gtksink",
            .stage = std::string(stage),
            .reason = std::string(reason),
        },
    };
}

std::optional<SsvEvent>
SsvRunFallbackState::try_software_decode_fallback(
    const SsvPipelinePlan &plan,
    const SsvRunAttemptResult &result,
    SsvEventContext failed_attempt)
{
    const bool runtime_contract_failure = result.stop_reason
        == SsvRunAttemptStopReason::PipelineContractFailure;
    const bool recoverable_creation_failure = result.stop_reason
            == SsvRunAttemptStopReason::PipelineStartFailure
        && (result.exit_code == SsvExitCode::CapabilityUnavailable
            || result.exit_code == SsvExitCode::PipelineContractFailed);
    if (software_decode_fallback_attempted_
        || (!runtime_contract_failure && !recoverable_creation_failure)
        || ssv_pipeline_contract_recovery(plan)
            != SsvPipelineContractRecovery::FallbackSoftware) {
        return std::nullopt;
    }

    software_decode_fallback_attempted_ = true;
    force_software_decode_ = true;
    return SsvEvent {
        .context = std::move(failed_attempt),
        .payload = SsvAccelerationFallbackEvent {
            .from = std::string(decode_backend_name(plan.decode.backend)),
            .to = "software",
            .stage = result.stage,
            .reason = result.error,
        },
    };
}

} // namespace ssv
