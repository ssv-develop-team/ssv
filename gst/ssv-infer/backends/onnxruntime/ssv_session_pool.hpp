#pragma once

#include "backends/onnxruntime/ssv_provider_resolver.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ssv::infer {

struct SsvSessionKey {
    std::string model_hash;
    std::vector<ssv::SsvProvider> provider_chain;
    int device_id = 0;
    ssv::SsvPrecision precision = ssv::SsvPrecision::Fp32;

    bool operator==(const SsvSessionKey &) const = default;
};

struct SsvSessionKeyHash {
    std::size_t operator()(const SsvSessionKey &key) const noexcept;
};

template <typename Session>
struct SsvSessionAcquireResult {
    std::shared_ptr<Session> session;
    bool reused = false;
};

template <typename Session>
class SsvSessionPool final {
public:
    using Factory = std::function<std::shared_ptr<Session>()>;

    [[nodiscard]] SsvSessionAcquireResult<Session> acquire(
        const SsvSessionKey &key,
        const Factory &factory)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = sessions_.find(key);
        if (found != sessions_.end()) {
            if (auto session = found->second.lock())
                return {std::move(session), true};
            sessions_.erase(found);
        }
        auto session = factory();
        if (!session)
            throw std::runtime_error("session pool factory returned null");
        sessions_.emplace(key, session);
        return {std::move(session), false};
    }

private:
    std::mutex mutex_;
    std::unordered_map<
        SsvSessionKey,
        std::weak_ptr<Session>,
        SsvSessionKeyHash> sessions_;
};

struct SsvCacheIdentity {
    SsvSessionKey session_key;
    std::vector<SsvProviderRegistration> provider_options;
    std::string dependency_signature;
    std::string device_identity;
    std::vector<std::pair<std::string, std::string>> runtime_versions;
};

struct SsvCacheLocation {
    SsvCacheStatus status = SsvCacheStatus::Disabled;
    std::string namespace_id;
    std::filesystem::path path;
    std::filesystem::path manifest_path;
};

struct SsvDeviceIdentity {
    std::string value;
    std::string unavailable_reason;
};

struct SsvNvidiaDeviceDescriptor {
    std::string pci_bus_id;
    std::string uuid;
    int compute_capability_major = -1;
    int compute_capability_minor = -1;
    std::string driver_version;
};

[[nodiscard]] std::string ssv_model_sha256(
    const std::filesystem::path &path);
[[nodiscard]] std::string ssv_session_key_id(const SsvSessionKey &key);
[[nodiscard]] std::string ssv_cache_namespace(
    const SsvCacheIdentity &identity);
[[nodiscard]] SsvCacheLocation ssv_cache_prepare(
    const ssv::SsvCacheConfig &config,
    const SsvCacheIdentity &identity,
    bool supports_native_cache);
[[nodiscard]] SsvCacheLocation ssv_cache_rebuild(
    const ssv::SsvCacheConfig &config,
    const SsvCacheIdentity &identity);
[[nodiscard]] SsvDeviceIdentity ssv_detect_device_identity(
    SsvRuntimeProfile profile,
    int device_id);
[[nodiscard]] bool ssv_drm_cache_identity_is_unambiguous(
    std::size_t device_count,
    int device_id) noexcept;
[[nodiscard]] SsvDeviceIdentity ssv_make_nvidia_device_identity(
    const SsvNvidiaDeviceDescriptor &descriptor);
[[nodiscard]] std::string_view ssv_cache_status_name(
    SsvCacheStatus status) noexcept;

} // namespace ssv::infer
