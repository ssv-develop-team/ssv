#include "ssv_config.hpp"
#include "observability/ssv_event_log.hpp"
#include "observability/ssv_stderr_log_sink.hpp"
#include "runtime/ssv_runner.hpp"

#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

struct ApplicationResult {
    int exit_code = static_cast<int>(ssv::SsvExitCode::Success);
    std::optional<ssv::SsvFatalError> fatal_error;
};

ApplicationResult fatal_result(ssv::SsvFatalError error)
{
    return {
        .exit_code = static_cast<int>(error.exit_code),
        .fatal_error = std::move(error),
    };
}

ApplicationResult run_application(
    int argc,
    char **argv,
    ssv::SsvEventLog &event_log)
{
    std::string source_id = "unresolved";
    try {
        std::vector<std::string_view> arguments;
        arguments.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
        for (int index = 1; index < argc; ++index)
            arguments.emplace_back(argv[index]);

        const auto options = ssv::ssv_run_options_parse(arguments);
        auto config = ssv::ssv_config_load(options.config_path);
        ssv::ssv_config_apply_overrides(config, options.overrides);
        source_id = config.sources.front().id;

        ssv::SsvRunner runner(std::move(config), event_log);
        auto result = runner.run();
        if (result.exit_code != ssv::SsvExitCode::Success) {
            return fatal_result({
                result.exit_code,
                std::move(result.stage),
                source_id,
                std::move(result.error),
            });
        }
        return {};
    } catch (const ssv::SsvConfigError &error) {
        return fatal_result(ssv::ssv_make_fatal_error(error, source_id));
    } catch (const std::exception &error) {
        return fatal_result({
            ssv::SsvExitCode::PipelineContractFailed,
            "pipeline.start",
            source_id,
            error.what(),
        });
    }
}

} // namespace

int main(int argc, char **argv)
{
    ssv::SsvEventLogOptions log_options;
    log_options.format = ::isatty(STDERR_FILENO) == 1
        ? ssv::SsvEventLogFormat::Pretty
        : ssv::SsvEventLogFormat::Structured;
    auto event_log = ssv::SsvEventLog::create(
        log_options, std::make_unique<ssv::SsvStderrLogSink>());
    auto result = run_application(argc, argv, *event_log);
    if (result.fatal_error) {
        auto fatal = std::move(*result.fatal_error);
        event_log->emit({
            .context = {
                .source_id = fatal.source_id,
                .run_attempt_id = std::nullopt,
            },
            .payload = ssv::SsvFatalErrorEvent {
                .exit_code = fatal.exit_code,
                .stage = std::move(fatal.stage),
                .error = std::move(fatal.error),
            },
        });
    }
    static_cast<void>(event_log->close());
    return result.exit_code;
}
