#pragma once

#include "ssv_config.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ssv::infer {

struct SsvInferenceStatsWindow;

enum class SsvProviderFailureStage {
    Configuration,
    Availability,
    Append,
    Session,
    Cache,
};

struct SsvProviderFallbackInfo {
    ssv::SsvProvider provider = ssv::SsvProvider::Cpu;
    SsvProviderFailureStage stage = SsvProviderFailureStage::Availability;
    std::string reason;
};

enum class SsvCacheStatus {
    Disabled,
    NotSupported,
    Unavailable,
    Miss,
    Hit,
    Rebuilt,
};

struct SsvNodePlacement {
    std::string node;
    std::string provider;

    bool operator==(const SsvNodePlacement &) const = default;
};

struct SsvInferenceStartupTimings {
    std::uint64_t provider_resolution_us = 0;
    std::uint64_t session_acquire_us = 0;
    std::uint64_t metadata_read_us = 0;
    std::uint64_t profiling_warmup_us = 0;
    std::uint64_t total_us = 0;
};

struct SsvInferenceStageTimings {
    // Empty means that this stage has no trustworthy measurement for the
    // sample; it must not be interpreted as zero work.
    std::optional<std::uint64_t> queue_us;
    std::optional<std::uint64_t> decode_us;
    std::optional<std::uint64_t> color_resize_us;
    std::optional<std::uint64_t> normalize_layout_us;
    std::optional<std::uint64_t> backend_h2d_us;
    std::optional<std::uint64_t> backend_execution_us;
    std::optional<std::uint64_t> backend_d2h_us;
    std::optional<std::uint64_t> backend_unattributed_us;
    std::optional<std::uint64_t> postprocess_us;
    std::optional<std::uint64_t> total_us;
};

struct SsvInferenceTimingSample {
    std::uint64_t completed_at_us = 0;
    SsvInferenceStageTimings timings;
};

struct SsvInferenceStatsWindowInput {
    std::uint64_t started_at_us = 0;
    std::uint64_t ended_at_us = 0;
    std::optional<std::uint64_t> previous_completion_us;
    std::uint64_t received = 0;
    std::uint64_t dropped = 0;
    std::vector<SsvInferenceTimingSample> completed_samples;
};

[[nodiscard]] SsvInferenceStatsWindow ssv_inference_stats_summarize(
    const SsvInferenceStatsWindowInput &input);

[[nodiscard]] std::vector<SsvNodePlacement> ssv_ort_profile_read(
    const std::filesystem::path &path);

} // namespace ssv::infer
