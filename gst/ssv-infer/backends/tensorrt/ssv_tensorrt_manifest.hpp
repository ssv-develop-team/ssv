#pragma once

#include "core/ssv_tensor.hpp"

#include <cstddef>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>

namespace ssv::infer {

class SsvTensorRtManifestError : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

struct SsvTensorRtRuntimeDescriptor {
    std::string tensorrt_version;
    int cuda_runtime_version = 0;
    int compute_capability_major = -1;
    int compute_capability_minor = -1;
};

struct SsvTensorRtEngineManifest {
    std::string engine_sha256;
    std::string source_model_sha256;
    std::string tensorrt_version;
    int cuda_runtime_version = 0;
    int compute_capability_major = -1;
    int compute_capability_minor = -1;
    ssv::SsvPrecision precision = ssv::SsvPrecision::Fp32;
    TensorSpec input;
};

[[nodiscard]] SsvTensorRtEngineManifest ssv_tensorrt_manifest_load_and_validate(
    const std::filesystem::path &manifest_path,
    std::span<const std::byte> engine_bytes,
    const SsvTensorRtRuntimeDescriptor &runtime);

void ssv_tensorrt_manifest_apply(
    const SsvTensorRtEngineManifest &manifest, ModelMetadata &metadata);

} // namespace ssv::infer
