#pragma once

#include "ssv_config.hpp"
#include "ssv_hardware_capabilities.hpp"
#include "ssv_model_contract.hpp"
#include "ssv_pipeline_contract.hpp"
#include "ssv_pipeline_plan.hpp"

#include <optional>
#include <string>
#include <vector>

namespace ssv::pipeline_internal {

struct PipelineStage {
    std::string factory;
    std::string name;
};

struct PipelineBranchTopology {
    std::vector<PipelineStage> stages;
    int queue_capacity = 1;
    bool leaky_downstream = true;
    bool drop_only = true;
    std::optional<int> max_rate;
};

struct PipelineTopology {
    // The RTSP source has a dynamic pad, so this path starts at the first
    // statically linkable element and ends at the encoded parser.
    std::vector<PipelineStage> source_path;
    std::optional<PipelineStage> encoded_tee;
    std::vector<PipelineStage> decode_path;
    std::optional<PipelineBranchTopology> display;
    std::optional<PipelineBranchTopology> analysis;
    std::optional<PipelineBranchTopology> evidence_cache;
    std::vector<std::string> required_factories;
    std::vector<SsvPipelineContractExpectation> contracts;
    std::string decode_caps;
    std::string display_upload_caps;
    std::string display_download_caps;
    std::string display_sink_caps;
    std::string display_overlay_caps;
    std::string display_encode_caps;
    std::string analysis_host_caps;
    bool depay_wait_for_keyframe = true;
    bool depay_request_keyframe = true;
};

[[nodiscard]] PipelineTopology resolve_topology(
    const SsvConfig &config,
    const SsvPipelinePlan &plan,
    const SsvHardwareCapabilities &registry,
    std::optional<infer::SsvModelContract> model_contract);

} // namespace ssv::pipeline_internal
