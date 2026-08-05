#pragma once

#include "ssv_config.hpp"
#include "observability/ssv_event.hpp"
#include "pipeline/ssv_pipeline_plan.hpp"
#include "ssv_run_result.hpp"

#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace ssv {

class SsvRunFallbackState {
public:
    [[nodiscard]] SsvConfig derive_effective_config(
        const SsvConfig &original_config) const;

    [[nodiscard]] std::optional<SsvEvent> take_creation_event(
        SsvEvent event);

    [[nodiscard]] std::vector<SsvEvent> take_plan_events(
        const SsvPipelinePlan &plan,
        const SsvEventContext &attempt_context);

    [[nodiscard]] std::optional<SsvEvent> try_display_fallback(
        const SsvPipelinePlan &plan,
        SsvEventContext failed_attempt,
        std::string_view stage,
        std::string_view reason);

    [[nodiscard]] std::optional<SsvEvent> try_software_decode_fallback(
        const SsvPipelinePlan &plan,
        const SsvRunAttemptResult &result,
        SsvEventContext failed_attempt);

private:
    bool display_fallback_attempted_ = false;
    bool force_gtk_sink_ = false;
    bool software_decode_fallback_attempted_ = false;
    bool force_software_decode_ = false;
    using FallbackEventKey = std::tuple<
        std::string,
        std::string,
        std::string,
        std::string,
        std::string>;
    std::set<FallbackEventKey> emitted_fallbacks_;
};

} // namespace ssv
