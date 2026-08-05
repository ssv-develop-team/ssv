#include "core/ssv_inference_stats.hpp"

#include "ssv_inference_service.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <functional>
#include <stdexcept>

namespace ssv::infer {
namespace {

SsvLatencyPercentiles percentiles(std::vector<std::uint64_t> values)
{
    if (values.empty())
        return {};
    std::ranges::sort(values);
    const auto nearest_rank = [&](std::size_t percentile) {
        const std::size_t rank =
            (values.size() * percentile + 99U) / 100U;
        return values[rank == 0 ? 0 : rank - 1];
    };
    return {nearest_rank(50), nearest_rank(95)};
}

template <typename Projection>
SsvLatencyPercentiles timing_percentiles(
    const std::vector<SsvInferenceTimingSample> &samples,
    Projection projection)
{
    std::vector<std::uint64_t> values;
    values.reserve(samples.size());
    for (const auto &sample : samples)
        values.push_back(std::invoke(projection, sample.timings));
    return percentiles(std::move(values));
}

} // namespace

SsvInferenceStatsWindow ssv_inference_stats_summarize(
    const SsvInferenceStatsWindowInput &input)
{
    if (input.ended_at_us < input.started_at_us) {
        throw std::invalid_argument(
            "inference stats window end precedes its start");
    }
    if (input.previous_completion_us
        && *input.previous_completion_us > input.started_at_us) {
        throw std::invalid_argument(
            "previous inference completion must not be inside the window");
    }

    auto samples = input.completed_samples;
    std::ranges::sort(samples, {},
        &SsvInferenceTimingSample::completed_at_us);
    std::uint64_t previous = input.previous_completion_us.value_or(
        input.started_at_us);
    std::uint64_t longest_gap = 0;
    for (const auto &sample : samples) {
        if (sample.completed_at_us < input.started_at_us
            || sample.completed_at_us > input.ended_at_us) {
            throw std::invalid_argument(
                "inference completion lies outside its stats window");
        }
        longest_gap = std::max(
            longest_gap, sample.completed_at_us - previous);
        previous = sample.completed_at_us;
    }
    longest_gap = std::max(longest_gap, input.ended_at_us - previous);

    const auto duration_us = input.ended_at_us - input.started_at_us;
    SsvInferenceStatsWindow result;
    result.received = input.received;
    result.dropped = input.dropped;
    result.completed = samples.size();
    result.completed_fps = duration_us == 0
        ? 0.0
        : static_cast<double>(result.completed) * 1'000'000.0
            / static_cast<double>(duration_us);
    result.longest_result_gap_us = longest_gap;
    result.queue = timing_percentiles(
        samples, &SsvInferenceStageTimings::queue_us);
    result.device = timing_percentiles(
        samples, &SsvInferenceStageTimings::device_us);
    result.output_copy = timing_percentiles(
        samples, &SsvInferenceStageTimings::output_copy_us);
    result.postprocess = timing_percentiles(
        samples, &SsvInferenceStageTimings::postprocess_us);
    result.total = timing_percentiles(
        samples, &SsvInferenceStageTimings::total_us);
    return result;
}

std::vector<SsvNodePlacement> ssv_ort_profile_read(
    const std::filesystem::path &path)
{
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error(
            "ONNX Runtime profiling output not found: " + path.string());

    nlohmann::json profile;
    input >> profile;
    if (!profile.is_array()) {
        throw std::runtime_error(
            "ONNX Runtime profiling output must be a JSON array");
    }

    std::vector<SsvNodePlacement> placements;
    for (const auto &event : profile) {
        if (!event.is_object() || event.value("cat", "") != "Node")
            continue;
        const auto args = event.find("args");
        if (args == event.end() || !args->is_object())
            continue;
        const auto provider = args->value("provider", "");
        if (provider.empty())
            continue;
        SsvNodePlacement placement {
            event.value("name", "unnamed"),
            provider,
        };
        if (std::find(placements.begin(), placements.end(), placement)
            == placements.end()) {
            placements.push_back(std::move(placement));
        }
    }
    return placements;
}

} // namespace ssv::infer
