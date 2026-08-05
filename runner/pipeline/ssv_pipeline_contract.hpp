#pragma once

#include "ssv_pipeline_plan.hpp"

#include <glib.h>
#include <gst/gst.h>

#include <optional>
#include <string>
#include <vector>

namespace ssv {

enum class SsvPipelineBoundary {
    DecodeTee,
    DisplayUpload,
    DisplaySink,
    AnalysisGpuInput,
    AnalysisHost,
};

struct SsvPipelineContractExpectation {
    SsvPipelineBoundary boundary;
    SsvPixelFormat format;
    std::vector<SsvMemoryKind> allowed_memories;
    int width = 0;
    int height = 0;
};

struct SsvPipelineContractObservation {
    std::optional<SsvPixelFormat> format;
    std::vector<SsvMemoryKind> caps_memories;
    std::optional<SsvMemoryKind> allocator_memory;
    std::vector<SsvMemoryKind> buffer_memories;
    int width = 0;
    int height = 0;
};

struct SsvPipelineContractViolation {
    SsvPipelineBoundary boundary;
    std::string expected_caps;
    std::string expected_allocator;
    std::string expected_memory;
    std::string actual_caps;
    std::string actual_allocator;
    std::string actual_memory;
    std::string message;
};

enum class SsvPipelineContractRecovery {
    Fatal,
    FallbackSoftware,
};

[[nodiscard]] std::optional<SsvPipelineContractViolation>
ssv_pipeline_contract_validate(
    const SsvPipelineContractExpectation &expectation,
    const SsvPipelineContractObservation &observation);

[[nodiscard]] SsvPipelineContractRecovery ssv_pipeline_contract_recovery(
    const SsvPipelinePlan &plan) noexcept;

[[nodiscard]] GQuark ssv_pipeline_contract_error_quark() noexcept;

[[nodiscard]] std::optional<SsvPipelineContractViolation>
ssv_pipeline_contract_violation_from_message(const GstMessage *message);

[[nodiscard]] std::optional<SsvPipelineBoundary>
ssv_pipeline_contract_ready_from_message(const GstMessage *message) noexcept;

} // namespace ssv
