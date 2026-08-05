#pragma once

#include "ssv_config.hpp"
#include "ssv_inference_service.hpp"
#include "display/ssv_window_lifecycle.hpp"
#include "pipeline/ssv_pipeline_instance.hpp"
#include "pipeline/ssv_pipeline_plan.hpp"
#include "ssv_run_result.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace ssv {

class SsvEventLog;

struct SsvRunAttemptOwnedResources {
    std::unique_ptr<SsvWindowLifecycle> window;
    infer::SsvInferenceServicePtr service;
};

struct SsvRunAttemptOptions {
    /// Borrowed; when non-null it must outlive the runner.
    SsvEventLog *event_log = nullptr;
    std::optional<std::uint32_t> run_attempt_id;
    /// Owning; present exactly when inference is enabled.
    std::optional<infer::SsvInferenceRuntimeSnapshot>
        inference_runtime_snapshot;
    std::chrono::milliseconds inference_stats_interval {5000};
};

enum class SsvRunAttemptState {
    Ready,
    Starting,
    Playing,
    Stopped,
};

class SsvRunAttempt {
public:
    SsvRunAttempt(
        SsvConfig config,
        SsvPipelinePlan plan,
        SsvPipelineInstance pipeline_instance,
        SsvRunAttemptOwnedResources resources = {},
        SsvRunAttemptOptions options = {});
    virtual ~SsvRunAttempt();

    SsvRunAttempt(const SsvRunAttempt &) = delete;
    SsvRunAttempt &operator=(const SsvRunAttempt &) = delete;

    [[nodiscard]] virtual SsvRunAttemptResult run();

    /// Must be called from the runner's owned main context.
    virtual void request_shutdown() noexcept;

    [[nodiscard]] virtual SsvRunAttemptState state() const noexcept;

protected:
    SsvRunAttempt() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ssv
