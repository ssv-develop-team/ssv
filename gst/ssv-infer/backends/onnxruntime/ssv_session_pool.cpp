#include "backends/onnxruntime/ssv_session_pool.hpp"

#include <glib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <dlfcn.h>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <stdexcept>

namespace ssv::infer {
namespace {

void hash_combine(std::size_t &seed, std::size_t value) noexcept
{
    seed ^= value + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
}

std::string sha256(std::string_view value)
{
    gchar *digest = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256,
        reinterpret_cast<const guchar *>(value.data()),
        value.size());
    if (digest == nullptr)
        throw std::runtime_error("failed to calculate SHA-256");
    std::string result(digest);
    g_free(digest);
    return result;
}

std::string precision_name(ssv::SsvPrecision precision)
{
    switch (precision) {
    case ssv::SsvPrecision::Auto: return "auto";
    case ssv::SsvPrecision::Fp32: return "fp32";
    case ssv::SsvPrecision::Fp16: return "fp16";
    }
    return "auto";
}

void append_component(
    std::ostringstream &output,
    std::string_view name,
    std::string_view value)
{
    output << name.size() << ':' << name << '='
           << value.size() << ':' << value << ';';
}

std::string session_key_canonical(const SsvSessionKey &key)
{
    std::ostringstream output;
    append_component(output, "model_hash", key.model_hash);
    output << "providers=";
    for (const auto provider : key.provider_chain)
        append_component(output, "provider", ssv_provider_name(provider));
    append_component(output, "device_id", std::to_string(key.device_id));
    append_component(output, "precision", precision_name(key.precision));
    return output.str();
}

std::string cache_identity_canonical(const SsvCacheIdentity &identity)
{
    std::ostringstream output;
    append_component(
        output, "session_key", session_key_canonical(identity.session_key));
    for (const auto &registration : identity.provider_options) {
        append_component(
            output, "provider", ssv_provider_name(registration.provider));
        auto options = registration.options;
        std::sort(options.begin(), options.end());
        for (const auto &[name, value] : options)
            append_component(output, name, value);
    }
    append_component(
        output, "dependency_signature", identity.dependency_signature);
    append_component(output, "device_identity", identity.device_identity);
    auto versions = identity.runtime_versions;
    std::sort(versions.begin(), versions.end());
    for (const auto &[name, version] : versions)
        append_component(output, name, version);
    return output.str();
}

bool lower_hex_sha256(std::string_view value)
{
    return value.size() == 64
        && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
            return std::isdigit(ch) != 0 || (ch >= 'a' && ch <= 'f');
        });
}

std::filesystem::path cache_root(const ssv::SsvCacheConfig &config)
{
    if (!config.directory.empty())
        return std::filesystem::path(config.directory).lexically_normal();
    if (const char *xdg = std::getenv("XDG_CACHE_HOME");
        xdg != nullptr && xdg[0] != '\0') {
        return (std::filesystem::path(xdg) / "ssv" / "ort")
            .lexically_normal();
    }
    if (const char *home = std::getenv("HOME");
        home != nullptr && home[0] != '\0') {
        return (std::filesystem::path(home) / ".cache" / "ssv" / "ort")
            .lexically_normal();
    }
    throw std::runtime_error(
        "cache directory is empty and neither XDG_CACHE_HOME nor HOME is set");
}

std::string expected_manifest(
    std::string_view namespace_id,
    std::string_view canonical)
{
    return "ssv-ort-cache\nnamespace=" + std::string(namespace_id)
        + "\nidentity=" + std::string(canonical) + "\n";
}

std::string read_text(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

void write_manifest(
    const std::filesystem::path &path,
    const std::string &contents)
{
    const auto temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
            throw std::runtime_error(
                "failed to create cache manifest: " + temporary);
        output << contents;
        if (!output)
            throw std::runtime_error(
                "failed to write cache manifest: " + temporary);
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary);
        throw std::runtime_error(
            "failed to publish cache manifest: " + error.message());
    }
}

std::string trim(std::string value)
{
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
        return {};
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::string line_value(const std::string &text, std::string_view key)
{
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        const auto separator = line.find(':');
        if (separator == std::string::npos)
            continue;
        if (trim(line.substr(0, separator)) == key)
            return trim(line.substr(separator + 1));
    }
    return {};
}

class DynamicLibrary final {
public:
    explicit DynamicLibrary(const char *name)
        : handle_(dlopen(name, RTLD_NOW | RTLD_LOCAL))
    {
    }

    ~DynamicLibrary()
    {
        if (handle_ != nullptr)
            dlclose(handle_);
    }

    DynamicLibrary(const DynamicLibrary &) = delete;
    DynamicLibrary &operator=(const DynamicLibrary &) = delete;

    [[nodiscard]] bool loaded() const noexcept
    {
        return handle_ != nullptr;
    }

    template <typename Function>
    [[nodiscard]] Function symbol(const char *name) const noexcept
    {
        if (handle_ == nullptr)
            return nullptr;
        return reinterpret_cast<Function>(dlsym(handle_, name));
    }

private:
    void *handle_ = nullptr;
};

using CudaResult = int;
using CudaDevice = int;

struct CudaUuid {
    char bytes[16];
};

using CudaInitialize = CudaResult (*)(unsigned int);
using CudaGetDevice = CudaResult (*)(CudaDevice *, int);
using CudaGetPciBusId = CudaResult (*)(char *, int, CudaDevice);
using CudaGetUuid = CudaResult (*)(CudaUuid *, CudaDevice);
using CudaGetDeviceAttribute =
    CudaResult (*)(int *, int, CudaDevice);

using NvmlInitialize = int (*)();
using NvmlShutdown = int (*)();
using NvmlGetDriverVersion = int (*)(char *, unsigned int);

constexpr CudaResult kCudaSuccess = 0;
constexpr int kCudaComputeCapabilityMajor = 75;
constexpr int kCudaComputeCapabilityMinor = 76;
constexpr int kNvmlSuccess = 0;

class NvmlShutdownGuard final {
public:
    explicit NvmlShutdownGuard(NvmlShutdown shutdown)
        : shutdown_(shutdown)
    {
    }

    ~NvmlShutdownGuard()
    {
        if (shutdown_ != nullptr)
            static_cast<void>(shutdown_());
    }

    NvmlShutdownGuard(const NvmlShutdownGuard &) = delete;
    NvmlShutdownGuard &operator=(const NvmlShutdownGuard &) = delete;

private:
    NvmlShutdown shutdown_;
};

std::string format_cuda_uuid(const CudaUuid &uuid)
{
    std::ostringstream output;
    output << "GPU-" << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < std::size(uuid.bytes); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10)
            output << '-';
        output << std::setw(2)
               << static_cast<unsigned int>(
                      static_cast<unsigned char>(uuid.bytes[index]));
    }
    return output.str();
}

std::string query_nvidia_driver_version()
{
    DynamicLibrary nvml("libnvidia-ml.so.1");
    if (!nvml.loaded())
        return {};
    auto initialize = nvml.symbol<NvmlInitialize>("nvmlInit_v2");
    if (initialize == nullptr)
        initialize = nvml.symbol<NvmlInitialize>("nvmlInit");
    const auto shutdown = nvml.symbol<NvmlShutdown>("nvmlShutdown");
    const auto get_driver_version =
        nvml.symbol<NvmlGetDriverVersion>("nvmlSystemGetDriverVersion");
    if (initialize == nullptr || shutdown == nullptr
        || get_driver_version == nullptr || initialize() != kNvmlSuccess) {
        return {};
    }
    NvmlShutdownGuard guard(shutdown);
    std::array<char, 96> version {};
    if (get_driver_version(version.data(),
            static_cast<unsigned int>(version.size())) != kNvmlSuccess) {
        return {};
    }
    return trim(version.data());
}

SsvDeviceIdentity nvidia_identity(int device_id)
{
    if (device_id < 0) {
        return {{}, "NVIDIA GPU identity requires a non-negative device_id"};
    }
    DynamicLibrary cuda("libcuda.so.1");
    if (!cuda.loaded())
        return {{}, "NVIDIA GPU identity cannot load libcuda.so.1"};

    const auto initialize = cuda.symbol<CudaInitialize>("cuInit");
    const auto get_device = cuda.symbol<CudaGetDevice>("cuDeviceGet");
    const auto get_pci_bus_id =
        cuda.symbol<CudaGetPciBusId>("cuDeviceGetPCIBusId");
    auto get_uuid = cuda.symbol<CudaGetUuid>("cuDeviceGetUuid_v2");
    if (get_uuid == nullptr)
        get_uuid = cuda.symbol<CudaGetUuid>("cuDeviceGetUuid");
    const auto get_attribute =
        cuda.symbol<CudaGetDeviceAttribute>("cuDeviceGetAttribute");
    if (initialize == nullptr || get_device == nullptr
        || get_pci_bus_id == nullptr || get_uuid == nullptr
        || get_attribute == nullptr) {
        return {{}, "NVIDIA GPU identity requires CUDA Driver identity APIs"};
    }
    if (initialize(0) != kCudaSuccess)
        return {{}, "NVIDIA GPU identity cannot initialize the CUDA Driver"};

    CudaDevice cuda_device = 0;
    if (get_device(&cuda_device, device_id) != kCudaSuccess) {
        return {{}, "NVIDIA GPU identity is unavailable for device_id="
                + std::to_string(device_id)};
    }
    std::array<char, 64> pci_bus_id {};
    CudaUuid uuid {};
    int compute_major = -1;
    int compute_minor = -1;
    if (get_pci_bus_id(
            pci_bus_id.data(),
            static_cast<int>(pci_bus_id.size()),
            cuda_device)
            != kCudaSuccess
        || get_uuid(&uuid, cuda_device) != kCudaSuccess
        || get_attribute(&compute_major,
               kCudaComputeCapabilityMajor, cuda_device)
            != kCudaSuccess
        || get_attribute(&compute_minor,
               kCudaComputeCapabilityMinor, cuda_device)
            != kCudaSuccess) {
        return {{}, "NVIDIA GPU identity query failed for device_id="
                + std::to_string(device_id)};
    }

    auto driver_version = query_nvidia_driver_version();
    if (driver_version.empty()) {
        driver_version = line_value(
            read_text("/proc/driver/nvidia/version"), "NVRM version");
    }
    return ssv_make_nvidia_device_identity({
        pci_bus_id.data(),
        format_cuda_uuid(uuid),
        compute_major,
        compute_minor,
        std::move(driver_version),
    });
}

SsvDeviceIdentity drm_identity(
    SsvRuntimeProfile profile,
    int device_id)
{
    const std::string expected_vendor =
        profile == SsvRuntimeProfile::Intel ? "0x8086" : "0x1002";
    std::vector<std::filesystem::path> matching_devices;
    const std::filesystem::path root("/sys/class/drm");
    std::error_code error;
    if (std::filesystem::is_directory(root, error)) {
        for (const auto &entry : std::filesystem::directory_iterator(root)) {
            const auto name = entry.path().filename().string();
            if (!name.starts_with("renderD"))
                continue;
            if (trim(read_text(entry.path() / "device/vendor"))
                == expected_vendor) {
                matching_devices.push_back(entry.path() / "device");
            }
        }
    }
    if (!ssv_drm_cache_identity_is_unambiguous(
            matching_devices.size(), device_id)) {
        return {{}, std::string(ssv_runtime_profile_name(profile))
                + " DRM cache identity is unavailable: cannot prove "
                  "provider device_id="
                + std::to_string(device_id)
                + " maps to one of "
                + std::to_string(matching_devices.size())
                + " matching render nodes"};
    }
    const auto &device_path = matching_devices.front();
    const auto canonical = std::filesystem::weakly_canonical(
        device_path, error);
    if (error)
        return {{}, "failed to resolve DRM device PCI identity"};
    const auto device = trim(read_text(device_path / "device"));
    const auto revision = trim(read_text(device_path / "revision"));
    const auto driver_version = trim(read_text(
        device_path / "driver/module/version"));
    return {
        std::string(ssv_runtime_profile_name(profile))
            + ":pci=" + canonical.filename().string()
            + ":device=" + device
            + ":revision=" + revision
            + ":driver=" + driver_version,
        {},
    };
}

} // namespace

bool ssv_drm_cache_identity_is_unambiguous(
    std::size_t device_count,
    int device_id) noexcept
{
    return device_count == 1U && device_id == 0;
}

SsvDeviceIdentity ssv_make_nvidia_device_identity(
    const SsvNvidiaDeviceDescriptor &descriptor)
{
    if (descriptor.pci_bus_id.empty() || descriptor.uuid.empty()
        || descriptor.compute_capability_major < 0
        || descriptor.compute_capability_minor < 0
        || descriptor.driver_version.empty()) {
        return {{}, "NVIDIA GPU cache identity is incomplete"};
    }
    return {
        "nvidia:pci=" + descriptor.pci_bus_id
            + ":uuid=" + descriptor.uuid
            + ":compute_capability="
            + std::to_string(descriptor.compute_capability_major) + "."
            + std::to_string(descriptor.compute_capability_minor)
            + ":driver=" + descriptor.driver_version,
        {},
    };
}

std::size_t SsvSessionKeyHash::operator()(
    const SsvSessionKey &key) const noexcept
{
    std::size_t seed = std::hash<std::string> {}(key.model_hash);
    for (const auto provider : key.provider_chain) {
        hash_combine(seed,
            std::hash<int> {}(static_cast<int>(provider)));
    }
    hash_combine(seed, std::hash<int> {}(key.device_id));
    hash_combine(seed,
        std::hash<int> {}(static_cast<int>(key.precision)));
    return seed;
}

std::string ssv_model_sha256(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("model file not found: " + path.string());

    GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
    if (checksum == nullptr)
        throw std::runtime_error("failed to allocate model checksum");
    try {
        char buffer[64 * 1024];
        while (input) {
            input.read(buffer, sizeof(buffer));
            const auto count = input.gcount();
            if (count > 0) {
                g_checksum_update(checksum,
                    reinterpret_cast<const guchar *>(buffer),
                    static_cast<gssize>(count));
            }
        }
        if (!input.eof())
            throw std::runtime_error("failed to read model: " + path.string());
        const char *digest = g_checksum_get_string(checksum);
        if (digest == nullptr)
            throw std::runtime_error("failed to finalize model checksum");
        std::string result(digest);
        g_checksum_free(checksum);
        return result;
    } catch (...) {
        g_checksum_free(checksum);
        throw;
    }
}

std::string ssv_session_key_id(const SsvSessionKey &key)
{
    return sha256(session_key_canonical(key));
}

std::string ssv_cache_namespace(const SsvCacheIdentity &identity)
{
    return sha256(cache_identity_canonical(identity));
}

SsvCacheLocation ssv_cache_prepare(
    const ssv::SsvCacheConfig &config,
    const SsvCacheIdentity &identity,
    bool supports_native_cache)
{
    if (!config.enabled)
        return {SsvCacheStatus::Disabled, {}, {}, {}};
    if (!supports_native_cache)
        return {SsvCacheStatus::NotSupported, {}, {}, {}};
    if (identity.dependency_signature.empty())
        return {SsvCacheStatus::Unavailable, {}, {}, {}};
    if (!lower_hex_sha256(identity.dependency_signature)) {
        throw std::runtime_error(
            "cache dependency signature must be a lowercase SHA-256");
    }
    if (identity.device_identity.empty())
        return {SsvCacheStatus::Unavailable, {}, {}, {}};

    const auto canonical = cache_identity_canonical(identity);
    const auto namespace_id = sha256(canonical);
    if (!lower_hex_sha256(namespace_id))
        throw std::logic_error("cache namespace must be a lowercase SHA-256");
    const auto root = cache_root(config);
    if (root.empty())
        throw std::runtime_error("cache root must not be empty");
    const auto path = (root / namespace_id).lexically_normal();
    if (path.parent_path() != root || path.filename() != namespace_id) {
        throw std::runtime_error("cache namespace escaped its configured root");
    }
    const auto manifest = path / "namespace.meta";
    const auto expected = expected_manifest(namespace_id, canonical);

    SsvCacheStatus status = SsvCacheStatus::Miss;
    std::error_code error;
    if (std::filesystem::exists(path, error)) {
        if (!error && read_text(manifest) == expected) {
            return {SsvCacheStatus::Hit, namespace_id, path, manifest};
        }
        if (!lower_hex_sha256(path.filename().string()))
            throw std::runtime_error("refusing to rebuild an unsafe cache path");
        std::filesystem::remove_all(path, error);
        if (error) {
            throw std::runtime_error(
                "failed to rebuild corrupt cache namespace: "
                + error.message());
        }
        status = SsvCacheStatus::Rebuilt;
    }

    std::filesystem::create_directories(path, error);
    if (error) {
        throw std::runtime_error(
            "failed to create cache namespace: " + error.message());
    }
    write_manifest(manifest, expected);
    return {status, namespace_id, path, manifest};
}

SsvCacheLocation ssv_cache_rebuild(
    const ssv::SsvCacheConfig &config,
    const SsvCacheIdentity &identity)
{
    if (identity.dependency_signature.empty())
        return {SsvCacheStatus::Unavailable, {}, {}, {}};
    if (!lower_hex_sha256(identity.dependency_signature)) {
        throw std::runtime_error(
            "cache dependency signature must be a lowercase SHA-256");
    }
    const auto root = cache_root(config);
    const auto namespace_id = ssv_cache_namespace(identity);
    if (!lower_hex_sha256(namespace_id))
        throw std::runtime_error("refusing to rebuild an unsafe cache namespace");
    const auto path = (root / namespace_id).lexically_normal();
    if (path.parent_path() != root || path.filename() != namespace_id)
        throw std::runtime_error("cache namespace escaped its configured root");
    std::error_code error;
    std::filesystem::remove_all(path, error);
    if (error) {
        throw std::runtime_error(
            "failed to rebuild cache namespace: " + error.message());
    }
    auto rebuilt = ssv_cache_prepare(config, identity, true);
    rebuilt.status = SsvCacheStatus::Rebuilt;
    return rebuilt;
}

SsvDeviceIdentity ssv_detect_device_identity(
    SsvRuntimeProfile profile,
    int device_id)
{
    switch (profile) {
    case SsvRuntimeProfile::Cpu:
        return {"cpu", {}};
    case SsvRuntimeProfile::Nvidia:
        return nvidia_identity(device_id);
    case SsvRuntimeProfile::Intel:
    case SsvRuntimeProfile::Amd:
        return drm_identity(profile, device_id);
    }
    return {{}, "unsupported runtime profile"};
}

std::string_view ssv_cache_status_name(SsvCacheStatus status) noexcept
{
    switch (status) {
    case SsvCacheStatus::Disabled: return "disabled";
    case SsvCacheStatus::NotSupported: return "not-supported";
    case SsvCacheStatus::Unavailable: return "unavailable";
    case SsvCacheStatus::Miss: return "miss";
    case SsvCacheStatus::Hit: return "hit";
    case SsvCacheStatus::Rebuilt: return "rebuilt";
    }
    return "unavailable";
}

} // namespace ssv::infer
