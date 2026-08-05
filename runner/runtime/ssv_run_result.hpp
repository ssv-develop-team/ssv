#pragma once

#include "ssv_config.hpp"

#include <string>

namespace ssv {

enum class SsvRunAttemptStopReason {
    EndOfStream,
    ControlledShutdown,
    PipelineStartFailure,
    PipelineContractFailure,
    RuntimeError,
};

struct SsvRunAttemptResult {
    SsvExitCode exit_code;
    SsvRunAttemptStopReason stop_reason;
    bool reached_playing;
    std::string stage;
    std::string error;
};

struct SsvRunnerResult {
    SsvExitCode exit_code = SsvExitCode::Success;
    std::string stage;
    std::string error;
};

} // namespace ssv
