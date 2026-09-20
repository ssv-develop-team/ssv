#pragma once

#include "ssv_config.hpp"
#include "ssv_hardware_capabilities.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace ssv {

enum class SsvDecodeBackend {
    Vaapi,
    Nvdec,
    Software,
};

enum class SsvResolvedDisplayBackend {
    GtkGlSink,
    GtkSink,
    RtspClientSink,
};

[[nodiscard]] constexpr bool ssv_display_backend_requires_window(
    SsvResolvedDisplayBackend backend) noexcept
{
    return backend == SsvResolvedDisplayBackend::GtkGlSink
        || backend == SsvResolvedDisplayBackend::GtkSink;
}

enum class SsvInferenceBackend {
    OnnxRuntime,
    TensorRtEngine,
};

enum class SsvPixelFormat {
    Nv12,
    Rgba,
    Bgrx,
    I420,
};

enum class SsvMemoryKind {
    SystemMemory,
    VaMemory,
    CudaMemory,
    DmaBuf,
    GlMemory,
    Unknown,
};

struct SsvVideoCaps {
    SsvPixelFormat format;
    SsvMemoryKind memory;

    bool operator==(const SsvVideoCaps &) const = default;
};

struct SsvPipelineExpectedCaps {
    SsvVideoCaps decode_output;
    std::optional<SsvVideoCaps> display_upload_input;
    std::optional<SsvVideoCaps> display_sink_input;
    std::optional<SsvVideoCaps> display_overlay_input;
    std::optional<SsvVideoCaps> display_encode_input;
    std::optional<SsvVideoCaps> analysis_gpu_input;
    std::optional<SsvVideoCaps> analysis_host_input;
};

struct SsvTrackingPlan {
    std::optional<int> nominal_frame_rate;
};

struct SsvDecodePlan {
    SsvDecodeBackend backend = SsvDecodeBackend::Software;
    SsvDecodeDevice device;
    std::string decoder_factory;
    std::string va_postproc_factory;
    bool software_fallback_allowed = false;
};

struct SsvInferencePlan {
    std::optional<SsvInferenceBackend> backend;
    std::vector<SsvProvider> available_providers;
};

enum class SsvEncodeBackend {
    Vaapi,
    Nvenc,
    Software,
};

enum class SsvCodecPath {
    Passthrough,
    DecodeOnly,
    HardwarePair,
    Mixed,
    Software,
};

struct SsvEncodePlan {
    SsvEncodeBackend backend = SsvEncodeBackend::Software;
    SsvDecodeDevice device;
    std::string encoder_factory;
    SsvMemoryKind input_memory = SsvMemoryKind::SystemMemory;
};

struct SsvCodecFallbackDecision {
    SsvCodecPath from = SsvCodecPath::HardwarePair;
    SsvCodecPath to = SsvCodecPath::Mixed;
    std::string reason;
};

struct SsvDecodeFallbackDecision {
    SsvDecodeBackend from;
    SsvDecodeBackend to;
    std::string reason;
};

struct SsvCodecPlan {
    SsvDecodePlan decode;
    std::optional<SsvEncodePlan> encode;
    SsvCodecPath path = SsvCodecPath::Passthrough;
    bool mixed_fallback_allowed = false;
    std::vector<SsvCodecFallbackDecision> fallbacks;
    std::vector<SsvDecodeFallbackDecision> decode_fallbacks;
    bool decoded_path_required = true;
};

struct SsvPipelineResolveOptions {
    bool force_mixed_codec = false;
};

class SsvPipelinePlanError : public std::runtime_error {
public:
    SsvPipelinePlanError(
        SsvExitCode exit_code,
        std::string stage,
        std::string message);

    [[nodiscard]] SsvExitCode exit_code() const noexcept;
    [[nodiscard]] const std::string &stage() const noexcept;

private:
    SsvExitCode exit_code_;
    std::string stage_;
};

struct SsvPipelinePlan {
    std::string source_id;
    SsvCapabilitySnapshot capability_snapshot;
    SsvInferencePlan inference;
    SsvCodecPlan codec;
    std::optional<SsvResolvedDisplayBackend> display_backend;
    bool display_encoded_passthrough = false;
    bool display_fallback_allowed = false;
    std::vector<std::string> display_fallback_reasons;
    std::optional<SsvTrackingPlan> tracking;
    SsvPipelineExpectedCaps expected_caps;

    [[nodiscard]] static SsvPipelinePlan resolve(
        const SsvConfig &config,
        const SsvCapabilitySnapshot &capabilities,
        SsvPipelineResolveOptions options = {});
};

} // namespace ssv
