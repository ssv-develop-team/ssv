#pragma once

#include "core/ssv_inference_engine.hpp"
#include "ssv_inference_service.hpp"

#include <functional>
#include <memory>
#include <stop_token>
#include <string_view>

namespace ssv::infer {

// Runs one in-flight task and keeps only the newest pending task. The executor
// is called exclusively by the scheduler worker and is not owned by callers.
class SsvLatestFrameScheduler final {
public:
    using Execute = std::function<SsvInferenceRunResult(
        const SsvInferenceRequest &,
        std::stop_token)>;

    explicit SsvLatestFrameScheduler(Execute execute);
    ~SsvLatestFrameScheduler();

    SsvLatestFrameScheduler(const SsvLatestFrameScheduler &) = delete;
    SsvLatestFrameScheduler &operator=(const SsvLatestFrameScheduler &) = delete;

    void start();

    [[nodiscard]] SsvInferenceSubmissionResult submit(
        SsvInferenceRequest request);

    void cancel(std::string_view source_id) noexcept;
    void stop() noexcept;

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] SsvInferenceStats stats() const noexcept;
    [[nodiscard]] SsvInferenceStatsWindow take_stats_window();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ssv::infer
