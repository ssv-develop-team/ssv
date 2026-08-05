#pragma once

#include "core/ssv_inference_stats.hpp"

#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ssv::infer {

enum class SsvRuntimeProfile {
    Cpu,
    Nvidia,
    Intel,
    Amd,
};

struct SsvSessionThreading {
    int intra_op_threads = 1;
    int inter_op_threads = 1;
    bool allow_spinning = false;
};

struct SsvProviderRegistration {
    ssv::SsvProvider provider = ssv::SsvProvider::Cpu;
    std::vector<std::pair<std::string, std::string>> options;

    bool operator==(const SsvProviderRegistration &) const = default;
};

struct SsvProviderAttempt {
    std::vector<SsvProviderRegistration> providers;
    ssv::SsvPrecision precision = ssv::SsvPrecision::Fp32;
    SsvSessionThreading threading;
    bool disable_cpu_fallback = false;
};

struct SsvProviderResolveRequest {
    SsvRuntimeProfile profile = SsvRuntimeProfile::Cpu;
    ssv::SsvProviderConfig providers;
    ssv::SsvPrecision precision = ssv::SsvPrecision::Auto;
    int device_id = 0;
    std::optional<int> cpu_threads;
    unsigned int logical_cpu_count = 0;
    std::vector<ssv::SsvProvider> available_providers;
};

struct SsvProviderResolution {
    SsvProviderAttempt active;
    std::vector<SsvProviderFallbackInfo> fallbacks;
};

class SsvProviderAttemptError : public std::runtime_error {
public:
    SsvProviderAttemptError(
        std::optional<ssv::SsvProvider> provider,
        SsvProviderFailureStage stage,
        std::string message);

    [[nodiscard]] std::optional<ssv::SsvProvider> provider() const noexcept;
    [[nodiscard]] SsvProviderFailureStage stage() const noexcept;

private:
    std::optional<ssv::SsvProvider> provider_;
    SsvProviderFailureStage stage_;
};

class SsvProviderResolutionError : public std::runtime_error {
public:
    SsvProviderResolutionError(
        std::optional<ssv::SsvProvider> provider,
        SsvProviderFailureStage stage,
        std::string message);

    [[nodiscard]] std::optional<ssv::SsvProvider> provider() const noexcept;
    [[nodiscard]] SsvProviderFailureStage stage() const noexcept;

private:
    std::optional<ssv::SsvProvider> provider_;
    SsvProviderFailureStage stage_;
};

using SsvProviderAttemptFunction =
    std::function<void(const SsvProviderAttempt &)>;

[[nodiscard]] SsvRuntimeProfile ssv_runtime_profile_parse(
    std::string_view value);
[[nodiscard]] std::string_view ssv_runtime_profile_name(
    SsvRuntimeProfile profile) noexcept;
[[nodiscard]] SsvRuntimeProfile ssv_build_runtime_profile();

[[nodiscard]] ssv::SsvProvider ssv_provider_parse(std::string_view value);
[[nodiscard]] std::string_view ssv_provider_name(
    ssv::SsvProvider provider) noexcept;
[[nodiscard]] std::string_view ssv_provider_runtime_name(
    ssv::SsvProvider provider) noexcept;
[[nodiscard]] std::string_view ssv_provider_failure_stage_name(
    SsvProviderFailureStage stage) noexcept;

[[nodiscard]] SsvProviderResolution ssv_provider_resolve(
    const SsvProviderResolveRequest &request,
    const SsvProviderAttemptFunction &attempt_session);

} // namespace ssv::infer
