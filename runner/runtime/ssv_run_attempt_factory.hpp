#pragma once

#include "observability/ssv_event.hpp"
#include "pipeline/ssv_hardware_capabilities.hpp"
#include "ssv_run_attempt.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace ssv {

class SsvEventLog;
class SsvRunner;

struct SsvRunAttemptCreation {
    std::unique_ptr<SsvRunAttempt> attempt;
    std::vector<SsvEvent> events;
};

class SsvRunAttemptFactoryError final : public std::runtime_error {
public:
    SsvRunAttemptFactoryError(
        SsvExitCode exit_code,
        std::string stage,
        std::string message);

    [[nodiscard]] SsvExitCode exit_code() const noexcept;
    [[nodiscard]] const std::string &stage() const noexcept;

private:
    SsvExitCode exit_code_;
    std::string stage_;
};

class SsvRunAttemptFactory {
public:
    virtual ~SsvRunAttemptFactory() = default;

    [[nodiscard]] virtual SsvHardwareCapabilities prepare_run(
        const SsvConfig &original_config) = 0;

    [[nodiscard]] virtual SsvRunAttemptCreation create(
        const SsvConfig &effective_config,
        const SsvPipelinePlan &plan,
        SsvEventContext context) = 0;
};

[[nodiscard]] std::unique_ptr<SsvRunAttemptFactory>
ssv_system_run_attempt_factory(SsvEventLog &event_log);

[[nodiscard]] std::unique_ptr<SsvRunner> ssv_runner_create_with_factory(
    SsvConfig config,
    SsvEventLog &event_log,
    std::unique_ptr<SsvRunAttemptFactory> attempt_factory);

} // namespace ssv
