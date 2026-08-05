#include "ssv_runner.hpp"

#include "display/ssv_display_window.hpp"
#include "observability/ssv_event_log.hpp"
#include "pipeline/ssv_pipeline_builder.hpp"
#include "pipeline/ssv_pipeline_plan.hpp"
#include "ssv_inference_service.hpp"
#include "ssv_run_attempt_factory.hpp"
#include "ssv_run_fallback_state.hpp"

#include <cstdint>
#include <exception>
#include <memory>
#include <utility>

namespace ssv {

class SsvRunner::Impl {
public:
    Impl(
        SsvConfig config,
        SsvEventLog &event_log,
        std::unique_ptr<SsvRunAttemptFactory> attempt_factory)
        : original_config_(std::move(config))
        , event_log_(event_log)
        , attempt_factory_(std::move(attempt_factory))
    {
    }

    [[nodiscard]] SsvRunnerResult run()
    {
        const auto capabilities =
            attempt_factory_->prepare_run(original_config_);
        SsvRunFallbackState fallback_state;
        std::uint32_t next_attempt_id = 1;

        while (true) {
            auto effective_config =
                fallback_state.derive_effective_config(original_config_);
            const auto plan =
                SsvPipelinePlan::resolve(effective_config, capabilities);
            SsvEventContext attempt_context {
                .source_id = plan.source_id,
                .run_attempt_id = next_attempt_id++,
            };
            for (auto &event :
                 fallback_state.take_plan_events(plan, attempt_context)) {
                event_log_.emit(std::move(event));
            }
            SsvRunAttemptResult attempt_result;
            try {
                auto creation = attempt_factory_->create(
                    effective_config, plan, attempt_context);
                for (auto &event : creation.events) {
                    auto accepted_event =
                        fallback_state.take_creation_event(std::move(event));
                    if (accepted_event)
                        event_log_.emit(std::move(*accepted_event));
                }
                attempt_result = creation.attempt->run();
            } catch (const SsvDisplayWindowError &error) {
                attempt_result = {
                    .exit_code = SsvExitCode::DisplayInitializationFailed,
                    .stop_reason =
                        SsvRunAttemptStopReason::PipelineStartFailure,
                    .reached_playing = false,
                    .stage = error.stage(),
                    .error = error.what(),
                };
            } catch (const SsvPipelineBuilderError &error) {
                attempt_result = {
                    .exit_code = error.exit_code(),
                    .stop_reason =
                        SsvRunAttemptStopReason::PipelineStartFailure,
                    .reached_playing = false,
                    .stage = error.stage(),
                    .error = error.what(),
                };
            } catch (const infer::SsvInferenceServiceError &error) {
                attempt_result = {
                    .exit_code = SsvExitCode::ModelInitializationFailed,
                    .stop_reason =
                        SsvRunAttemptStopReason::PipelineStartFailure,
                    .reached_playing = false,
                    .stage = error.stage(),
                    .error = error.what(),
                };
            }

            if (attempt_result.exit_code != SsvExitCode::Success) {
                auto fallback_event = fallback_state.try_display_fallback(
                    plan,
                    attempt_context,
                    attempt_result.stage,
                    attempt_result.error);
                if (fallback_event) {
                    event_log_.emit(std::move(*fallback_event));
                    continue;
                }

                fallback_event =
                    fallback_state.try_software_decode_fallback(
                        plan, attempt_result, attempt_context);
                if (fallback_event) {
                    event_log_.emit(std::move(*fallback_event));
                    continue;
                }
            }

            return {
                .exit_code = attempt_result.exit_code,
                .stage = std::move(attempt_result.stage),
                .error = std::move(attempt_result.error),
            };
        }
    }

private:
    SsvConfig original_config_;
    SsvEventLog &event_log_;
    std::unique_ptr<SsvRunAttemptFactory> attempt_factory_;
};

SsvRunner::SsvRunner(
    SsvConfig config,
    SsvEventLog &event_log)
    : SsvRunner(
        std::move(config),
        event_log,
        ssv_system_run_attempt_factory(event_log))
{
}

SsvRunner::SsvRunner(
    SsvConfig config,
    SsvEventLog &event_log,
    std::unique_ptr<SsvRunAttemptFactory> attempt_factory)
    : impl_(std::make_unique<Impl>(
        std::move(config), event_log, std::move(attempt_factory)))
{
}

SsvRunner::~SsvRunner() = default;

SsvRunnerResult SsvRunner::run()
{
    try {
        return impl_->run();
    } catch (const SsvPipelinePlanError &error) {
        return {
            .exit_code = error.exit_code(),
            .stage = error.stage(),
            .error = error.what(),
        };
    } catch (const SsvRunAttemptFactoryError &error) {
        return {
            .exit_code = error.exit_code(),
            .stage = error.stage(),
            .error = error.what(),
        };
    } catch (const SsvPipelineBuilderError &error) {
        return {
            .exit_code = error.exit_code(),
            .stage = error.stage(),
            .error = error.what(),
        };
    } catch (const SsvDisplayWindowError &error) {
        return {
            .exit_code = SsvExitCode::DisplayInitializationFailed,
            .stage = error.stage(),
            .error = error.what(),
        };
    } catch (const infer::SsvInferenceServiceError &error) {
        return {
            .exit_code = SsvExitCode::ModelInitializationFailed,
            .stage = error.stage(),
            .error = error.what(),
        };
    } catch (const std::exception &error) {
        return {
            .exit_code = SsvExitCode::PipelineContractFailed,
            .stage = "pipeline.start",
            .error = error.what(),
        };
    }
}

std::unique_ptr<SsvRunner> ssv_runner_create_with_factory(
    SsvConfig config,
    SsvEventLog &event_log,
    std::unique_ptr<SsvRunAttemptFactory> attempt_factory)
{
    return std::unique_ptr<SsvRunner>(new SsvRunner(
        std::move(config), event_log, std::move(attempt_factory)));
}

} // namespace ssv
