#pragma once

#include "ssv_config.hpp"
#include "ssv_run_result.hpp"

#include <memory>

namespace ssv {

class SsvEventLog;
class SsvRunAttemptFactory;

class SsvRunner {
public:
    SsvRunner(SsvConfig config, SsvEventLog &event_log);
    ~SsvRunner();

    SsvRunner(const SsvRunner &) = delete;
    SsvRunner &operator=(const SsvRunner &) = delete;

    [[nodiscard]] SsvRunnerResult run();

private:
    SsvRunner(
        SsvConfig config,
        SsvEventLog &event_log,
        std::unique_ptr<SsvRunAttemptFactory> attempt_factory);

    friend std::unique_ptr<SsvRunner> ssv_runner_create_with_factory(
        SsvConfig config,
        SsvEventLog &event_log,
        std::unique_ptr<SsvRunAttemptFactory> attempt_factory);

    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ssv
