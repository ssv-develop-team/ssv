#pragma once

#include "ssv_config.hpp"
#include "observability/ssv_event.hpp"
#include "pipeline/ssv_pipeline_contract.hpp"
#include "pipeline/ssv_pipeline_plan.hpp"
#include "ssv_inference_service.hpp"

#include <gst/gst.h>

#include <optional>
#include <vector>

namespace ssv {

SsvEvent ssv_runtime_resolved_event(
    SsvEventContext context,
    const SsvConfig &config,
    const SsvPipelinePlan &plan,
    const std::optional<infer::SsvInferenceRuntimeSnapshot>
        &inference_runtime_snapshot);

std::vector<SsvEvent> ssv_provider_fallback_events(
    const SsvEventContext &context,
    const infer::SsvInferenceRuntimeSnapshot &snapshot);

SsvEvent ssv_buffer_contract_failed_event(
    SsvEventContext context,
    std::string stage,
    const SsvPipelineContractViolation &violation);

SsvEvent ssv_inference_stats_event(
    SsvEventContext context,
    const infer::SsvInferenceStatsWindow &stats);

SsvEvent ssv_gstreamer_warning_event(
    SsvEventContext context,
    GstMessage *message);

} // namespace ssv
