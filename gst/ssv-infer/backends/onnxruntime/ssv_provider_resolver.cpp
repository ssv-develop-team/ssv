#include "backends/onnxruntime/ssv_provider_resolver.hpp"

#include <algorithm>
#include <limits>
#include <thread>

#ifndef SSV_ONNXRUNTIME_PROFILE
#define SSV_ONNXRUNTIME_PROFILE "cpu"
#endif

namespace ssv::infer {
namespace {

using ProviderChain = std::vector<ssv::SsvProvider>;

ProviderChain auto_chain(SsvRuntimeProfile profile)
{
    switch (profile) {
    case SsvRuntimeProfile::Cpu:
        return {ssv::SsvProvider::Cpu};
    case SsvRuntimeProfile::Nvidia:
        return {
            ssv::SsvProvider::TensorRt,
            ssv::SsvProvider::Cuda,
            ssv::SsvProvider::Cpu,
        };
    case SsvRuntimeProfile::Intel:
        return {ssv::SsvProvider::OpenVino, ssv::SsvProvider::Cpu};
    case SsvRuntimeProfile::Amd:
        return {ssv::SsvProvider::MiGraphX, ssv::SsvProvider::Cpu};
    }
    return {};
}

bool contains(const ProviderChain &providers, ssv::SsvProvider provider)
{
    return std::find(providers.begin(), providers.end(), provider)
        != providers.end();
}

bool is_gpu(ssv::SsvProvider provider)
{
    return provider != ssv::SsvProvider::Cpu;
}

bool supports_fp16_conversion(ssv::SsvProvider provider)
{
    return provider == ssv::SsvProvider::TensorRt
        || provider == ssv::SsvProvider::OpenVino
        || provider == ssv::SsvProvider::MiGraphX;
}

void validate_requested_chain(const ProviderChain &providers)
{
    if (providers.empty()) {
        throw SsvProviderResolutionError(
            std::nullopt,
            SsvProviderFailureStage::Configuration,
            "explicit Provider order must not be empty");
    }
    ProviderChain seen;
    for (const auto provider : providers) {
        if (contains(seen, provider)) {
            throw SsvProviderResolutionError(
                provider,
                SsvProviderFailureStage::Configuration,
                "Provider order contains a duplicate alias: "
                    + std::string(ssv_provider_name(provider)));
        }
        seen.push_back(provider);
    }
    const auto cpu = std::find(
        providers.begin(), providers.end(), ssv::SsvProvider::Cpu);
    if (cpu != providers.end() && std::next(cpu) != providers.end()) {
        throw SsvProviderResolutionError(
            ssv::SsvProvider::Cpu,
            SsvProviderFailureStage::Configuration,
            "CPU Provider must be the final explicit Provider");
    }
}

ssv::SsvPrecision resolve_precision(
    ssv::SsvPrecision requested,
    const ProviderChain &providers)
{
    const auto accelerated = std::find_if(
        providers.begin(), providers.end(), is_gpu);
    const auto leading_provider = accelerated == providers.end()
        ? ssv::SsvProvider::Cpu
        : *accelerated;
    const bool supports_fp16 = supports_fp16_conversion(leading_provider);
    if (requested == ssv::SsvPrecision::Auto)
        return supports_fp16
            ? ssv::SsvPrecision::Fp16
            : ssv::SsvPrecision::Fp32;
    if (requested == ssv::SsvPrecision::Fp16 && !supports_fp16) {
        throw SsvProviderResolutionError(
            leading_provider,
            SsvProviderFailureStage::Configuration,
            "precision=fp16 requires TensorRT, OpenVINO GPU, or MIGraphX "
            "as the leading Provider");
    }
    return requested;
}

SsvSessionThreading resolve_threading(
    const SsvProviderResolveRequest &request,
    const ProviderChain &providers)
{
    const bool has_gpu = std::any_of(
        providers.begin(), providers.end(), is_gpu);
    if (has_gpu)
        return {1, 1, false};
    if (request.cpu_threads)
        return {*request.cpu_threads, 1, false};

    const auto logical_cpus = request.logical_cpu_count == 0
        ? std::max(1U, std::thread::hardware_concurrency())
        : request.logical_cpu_count;
    const auto available = logical_cpus > 2 ? logical_cpus - 2 : 1U;
    const auto capped = std::min(available, 8U);
    return {static_cast<int>(capped), 1, false};
}

SsvProviderRegistration registration_for(
    ssv::SsvProvider provider,
    ssv::SsvPrecision precision,
    int device_id)
{
    SsvProviderRegistration registration;
    registration.provider = provider;
    const auto device = std::to_string(device_id);
    switch (provider) {
    case ssv::SsvProvider::TensorRt:
        registration.options = {
            {"device_id", device},
            {"trt_fp16_enable",
                precision == ssv::SsvPrecision::Fp16 ? "1" : "0"},
        };
        break;
    case ssv::SsvProvider::Cuda:
        registration.options = {{"device_id", device}};
        break;
    case ssv::SsvProvider::OpenVino:
        registration.options = {
            {"device_type", "GPU." + device},
            {"precision",
                precision == ssv::SsvPrecision::Fp16 ? "FP16" : "FP32"},
        };
        break;
    case ssv::SsvProvider::MiGraphX:
        registration.options = {
            {"device_id", device},
            {"migraphx_fp16_enable",
                precision == ssv::SsvPrecision::Fp16 ? "1" : "0"},
        };
        break;
    case ssv::SsvProvider::Cpu:
        break;
    }
    return registration;
}

SsvProviderAttempt make_attempt(
    const SsvProviderResolveRequest &request,
    const ProviderChain &providers)
{
    SsvProviderAttempt attempt;
    attempt.precision = resolve_precision(request.precision, providers);
    attempt.threading = resolve_threading(request, providers);
    attempt.disable_cpu_fallback =
        request.providers.mode == ssv::SsvProviderMode::Explicit
        && !contains(providers, ssv::SsvProvider::Cpu);
    for (const auto provider : providers) {
        attempt.providers.push_back(registration_for(
            provider, attempt.precision, request.device_id));
    }
    return attempt;
}

std::optional<ssv::SsvProvider> fallback_provider(
    const ProviderChain &providers,
    std::optional<ssv::SsvProvider> failed)
{
    if (failed && *failed != ssv::SsvProvider::Cpu
        && contains(providers, *failed)) {
        return failed;
    }
    const auto accelerated = std::find_if(
        providers.begin(), providers.end(), is_gpu);
    if (accelerated == providers.end())
        return std::nullopt;
    return *accelerated;
}

[[noreturn]] void fail_attempt(const SsvProviderAttemptError &error)
{
    throw SsvProviderResolutionError(
        error.provider(), error.stage(), error.what());
}

} // namespace

SsvProviderAttemptError::SsvProviderAttemptError(
    std::optional<ssv::SsvProvider> provider,
    SsvProviderFailureStage stage,
    std::string message)
    : std::runtime_error(std::move(message))
    , provider_(provider)
    , stage_(stage)
{
}

std::optional<ssv::SsvProvider>
SsvProviderAttemptError::provider() const noexcept
{
    return provider_;
}

SsvProviderFailureStage SsvProviderAttemptError::stage() const noexcept
{
    return stage_;
}

SsvProviderResolutionError::SsvProviderResolutionError(
    std::optional<ssv::SsvProvider> provider,
    SsvProviderFailureStage stage,
    std::string message)
    : std::runtime_error(std::move(message))
    , provider_(provider)
    , stage_(stage)
{
}

std::optional<ssv::SsvProvider>
SsvProviderResolutionError::provider() const noexcept
{
    return provider_;
}

SsvProviderFailureStage SsvProviderResolutionError::stage() const noexcept
{
    return stage_;
}

SsvRuntimeProfile ssv_runtime_profile_parse(std::string_view value)
{
    if (value == "cpu")
        return SsvRuntimeProfile::Cpu;
    if (value == "nvidia")
        return SsvRuntimeProfile::Nvidia;
    if (value == "intel")
        return SsvRuntimeProfile::Intel;
    if (value == "amd")
        return SsvRuntimeProfile::Amd;
    throw SsvProviderResolutionError(
        std::nullopt,
        SsvProviderFailureStage::Configuration,
        "resolved runtime profile must be cpu, nvidia, intel, or amd");
}

std::string_view ssv_runtime_profile_name(
    SsvRuntimeProfile profile) noexcept
{
    switch (profile) {
    case SsvRuntimeProfile::Cpu: return "cpu";
    case SsvRuntimeProfile::Nvidia: return "nvidia";
    case SsvRuntimeProfile::Intel: return "intel";
    case SsvRuntimeProfile::Amd: return "amd";
    }
    return "cpu";
}

SsvRuntimeProfile ssv_build_runtime_profile()
{
    return ssv_runtime_profile_parse(SSV_ONNXRUNTIME_PROFILE);
}

ssv::SsvProvider ssv_provider_parse(std::string_view value)
{
    if (value == "tensorrt")
        return ssv::SsvProvider::TensorRt;
    if (value == "cuda")
        return ssv::SsvProvider::Cuda;
    if (value == "openvino")
        return ssv::SsvProvider::OpenVino;
    if (value == "migraphx")
        return ssv::SsvProvider::MiGraphX;
    if (value == "cpu")
        return ssv::SsvProvider::Cpu;
    throw SsvProviderResolutionError(
        std::nullopt,
        SsvProviderFailureStage::Configuration,
        "unsupported Provider alias: " + std::string(value));
}

std::string_view ssv_provider_name(ssv::SsvProvider provider) noexcept
{
    switch (provider) {
    case ssv::SsvProvider::TensorRt: return "tensorrt";
    case ssv::SsvProvider::Cuda: return "cuda";
    case ssv::SsvProvider::OpenVino: return "openvino";
    case ssv::SsvProvider::MiGraphX: return "migraphx";
    case ssv::SsvProvider::Cpu: return "cpu";
    }
    return "cpu";
}

std::string_view ssv_provider_runtime_name(
    ssv::SsvProvider provider) noexcept
{
    switch (provider) {
    case ssv::SsvProvider::TensorRt: return "TensorrtExecutionProvider";
    case ssv::SsvProvider::Cuda: return "CUDAExecutionProvider";
    case ssv::SsvProvider::OpenVino: return "OpenVINOExecutionProvider";
    case ssv::SsvProvider::MiGraphX: return "MIGraphXExecutionProvider";
    case ssv::SsvProvider::Cpu: return "CPUExecutionProvider";
    }
    return "CPUExecutionProvider";
}

std::string_view ssv_provider_failure_stage_name(
    SsvProviderFailureStage stage) noexcept
{
    switch (stage) {
    case SsvProviderFailureStage::Configuration: return "configuration";
    case SsvProviderFailureStage::Availability: return "availability";
    case SsvProviderFailureStage::Append: return "append";
    case SsvProviderFailureStage::Session: return "session";
    case SsvProviderFailureStage::Cache: return "cache";
    }
    return "configuration";
}

SsvProviderResolution ssv_provider_resolve(
    const SsvProviderResolveRequest &request,
    const SsvProviderAttemptFunction &attempt_session)
{
    if (!attempt_session) {
        throw SsvProviderResolutionError(
            std::nullopt,
            SsvProviderFailureStage::Configuration,
            "Provider session attempt callback must be set");
    }
    if (request.device_id < 0) {
        throw SsvProviderResolutionError(
            std::nullopt,
            SsvProviderFailureStage::Configuration,
            "Provider device_id must not be negative");
    }

    const bool explicit_mode =
        request.providers.mode == ssv::SsvProviderMode::Explicit;
    ProviderChain requested = explicit_mode
        ? request.providers.order
        : auto_chain(request.profile);
    validate_requested_chain(requested);
    if (!explicit_mode && !contains(requested, ssv::SsvProvider::Cpu))
        requested.push_back(ssv::SsvProvider::Cpu);

    SsvProviderResolution resolution;
    ProviderChain candidates;
    for (const auto provider : requested) {
        if (contains(request.available_providers, provider)) {
            candidates.push_back(provider);
            continue;
        }
        const std::string reason =
            std::string(ssv_provider_runtime_name(provider))
            + " is not available in the active "
            + std::string(ssv_runtime_profile_name(request.profile))
            + " runtime profile";
        if (explicit_mode || provider == ssv::SsvProvider::Cpu) {
            throw SsvProviderResolutionError(
                provider, SsvProviderFailureStage::Availability, reason);
        }
        resolution.fallbacks.push_back({
            provider,
            SsvProviderFailureStage::Availability,
            reason,
        });
    }

    while (!candidates.empty()) {
        resolution.active = make_attempt(request, candidates);
        try {
            attempt_session(resolution.active);
            return resolution;
        } catch (const SsvProviderAttemptError &error) {
            if (explicit_mode)
                fail_attempt(error);
            const auto provider = fallback_provider(
                candidates, error.provider());
            if (!provider)
                fail_attempt(error);
            resolution.fallbacks.push_back({
                *provider,
                error.stage(),
                error.what(),
            });
            candidates.erase(std::remove(
                candidates.begin(), candidates.end(), *provider),
                candidates.end());
        } catch (const std::exception &error) {
            const SsvProviderAttemptError wrapped(
                std::nullopt,
                SsvProviderFailureStage::Session,
                error.what());
            if (explicit_mode)
                fail_attempt(wrapped);
            const auto provider = fallback_provider(candidates, std::nullopt);
            if (!provider)
                fail_attempt(wrapped);
            resolution.fallbacks.push_back({
                *provider,
                SsvProviderFailureStage::Session,
                error.what(),
            });
            candidates.erase(std::remove(
                candidates.begin(), candidates.end(), *provider),
                candidates.end());
        }
    }

    throw SsvProviderResolutionError(
        std::nullopt,
        SsvProviderFailureStage::Availability,
        "no ONNX Runtime Provider remains after resolution");
}

} // namespace ssv::infer
