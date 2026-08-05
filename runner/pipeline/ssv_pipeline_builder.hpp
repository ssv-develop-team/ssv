#pragma once

#include "ssv_hardware_capabilities.hpp"
#include "ssv_inference_service.hpp"
#include "ssv_pipeline_instance.hpp"
#include "ssv_pipeline_plan.hpp"

#include <stdexcept>
#include <string>

namespace ssv {

class SsvPipelineBuilderError : public std::runtime_error {
public:
    SsvPipelineBuilderError(
        SsvExitCode exit_code,
        std::string stage,
        std::string message);

    [[nodiscard]] SsvExitCode exit_code() const noexcept;
    [[nodiscard]] const std::string &stage() const noexcept;

private:
    SsvExitCode exit_code_;
    std::string stage_;
};

class SsvPipelineBuilder final {
public:
    [[nodiscard]] static SsvPipelineInstance build(
        const SsvConfig &config,
        const SsvPipelinePlan &plan,
        const SsvHardwareCapabilities &registry,
        SsvInferenceService *inference_service);
};

} // namespace ssv
