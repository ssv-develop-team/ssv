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
    SsvDecodeBackend backend;
    SsvDecodeDevice device;
    std::string decoder_factory;
    std::string va_postproc_factory;
    bool software_fallback_allowed = false;
};

struct SsvDecodeFallbackDecision {
    SsvDecodeBackend from;
    SsvDecodeBackend to;
    std::string reason;
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
    SsvDecodePlan decode;
    std::vector<SsvDecodeFallbackDecision> decode_fallbacks;
    std::optional<SsvResolvedDisplayBackend> display_backend;
    bool display_encoded_passthrough = false;
    bool decoded_path_required = true;
    std::string display_encoder_factory;
    bool display_fallback_allowed = false;
    std::vector<std::string> display_fallback_reasons;
    std::optional<SsvInferenceBackend> inference_backend;
    std::optional<SsvTrackingPlan> tracking;
    SsvPipelineExpectedCaps expected_caps;

    [[nodiscard]] static SsvPipelinePlan resolve(
        const SsvConfig &config,
        const SsvCapabilitySnapshot &capabilities);

    [[nodiscard]] static SsvPipelinePlan resolve(
        const SsvConfig &config,
        const SsvHardwareCapabilities &capabilities);
};

} // namespace ssv
