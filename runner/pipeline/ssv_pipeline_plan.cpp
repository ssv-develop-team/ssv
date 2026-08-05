#include "ssv_pipeline_plan.hpp"

#include <cctype>
#include <string_view>
#include <variant>

namespace ssv {
namespace {

constexpr auto kGtkGlDmabufRequirement =
    "gtkglsink requires DMABuf input from the resolved decoder";

bool is_blank(std::string_view value)
{
    for (const unsigned char character : value) {
        if (!std::isspace(character))
            return false;
    }
    return true;
}

[[noreturn]] void fail_plan(
    SsvExitCode exit_code,
    std::string stage,
    std::string message)
{
    throw SsvPipelinePlanError(
        exit_code, std::move(stage), std::move(message));
}

std::string va_device_factory(
    const SsvDecodeDevice &device,
    std::string_view suffix)
{
    if (device.kind != SsvDecodeDeviceKind::Drm)
        return {};
    const auto separator = device.value.find_last_of('/');
    const auto basename = separator == std::string::npos
        ? device.value
        : device.value.substr(separator + 1);
    return "va" + basename + std::string(suffix);
}

std::string nvdec_factory(const SsvDecodeDevice &device)
{
    if (device.kind != SsvDecodeDeviceKind::Cuda || device.value == "0")
        return "nvh264dec";
    return "nvh264device" + device.value + "dec";
}

std::string require_factory(
    const SsvHardwareCapabilities &capabilities,
    std::string preferred,
    std::string fallback,
    std::string_view purpose)
{
    if (!preferred.empty()
        && capabilities.has_gstreamer_element(preferred)) {
        return preferred;
    }
    if (!fallback.empty()
        && capabilities.has_gstreamer_element(fallback)) {
        return fallback;
    }
    const auto missing = preferred.empty() ? fallback : preferred;
    fail_plan(
        SsvExitCode::CapabilityUnavailable,
        "capability.decode",
        "required GStreamer " + std::string(purpose)
            + " is unavailable: " + missing);
}

bool has_factory(
    const SsvHardwareCapabilities &capabilities,
    std::string_view preferred,
    std::string_view fallback = {})
{
    const bool preferred_available = !preferred.empty()
        && capabilities.has_gstreamer_element(preferred);
    const bool fallback_available = !fallback.empty()
        && capabilities.has_gstreamer_element(fallback);
    return preferred_available || fallback_available;
}

SsvDecodePlan make_vaapi_plan(
    const SsvDecodeConfig &decode,
    const SsvHardwareCapabilities &capabilities,
    bool software_fallback_allowed)
{
    return {
        .backend = SsvDecodeBackend::Vaapi,
        .device = decode.device,
        .decoder_factory = require_factory(
            capabilities,
            va_device_factory(decode.device, "h264dec"),
            "vah264dec",
            "VA H.264 decoder"),
        .va_postproc_factory = require_factory(
            capabilities,
            va_device_factory(decode.device, "postproc"),
            "vapostproc",
            "VA post-processor"),
        .software_fallback_allowed = software_fallback_allowed,
    };
}

SsvDecodePlan make_nvdec_plan(
    const SsvDecodeConfig &decode,
    const SsvHardwareCapabilities &capabilities,
    bool software_fallback_allowed)
{
    const auto factory = nvdec_factory(decode.device);
    return {
        .backend = SsvDecodeBackend::Nvdec,
        .device = decode.device,
        .decoder_factory = require_factory(
            capabilities, factory, {}, "NVDEC H.264 decoder"),
        .va_postproc_factory = {},
        .software_fallback_allowed = software_fallback_allowed,
    };
}

SsvDecodePlan make_software_plan(
    const SsvDecodeConfig &decode,
    const SsvHardwareCapabilities &capabilities,
    bool software_fallback_allowed)
{
    return {
        .backend = SsvDecodeBackend::Software,
        .device = decode.device,
        .decoder_factory = require_factory(
            capabilities, "avdec_h264", {}, "software H.264 decoder"),
        .va_postproc_factory = {},
        .software_fallback_allowed = software_fallback_allowed,
    };
}

SsvDecodePlan make_software_fallback_plan(
    const SsvDecodeConfig &decode,
    const SsvHardwareCapabilities &capabilities)
{
    auto software = decode;
    software.mode = SsvDecodeMode::Software;
    software.device = {};
    return make_software_plan(software, capabilities, true);
}

SsvDecodePlan resolve_decode_plan(
    const SsvDecodeConfig &decode,
    const SsvHardwareCapabilities &capabilities)
{
    const bool allow_software_fallback =
        decode.mode == SsvDecodeMode::Auto;

    switch (decode.mode) {
    case SsvDecodeMode::Auto:
        if (decode.device.kind == SsvDecodeDeviceKind::Drm) {
            const auto decoder = va_device_factory(
                decode.device, "h264dec");
            const auto postproc = va_device_factory(
                decode.device, "postproc");
            if (has_factory(capabilities, decoder, "vah264dec")
                && has_factory(capabilities, postproc, "vapostproc")) {
                return make_vaapi_plan(
                    decode, capabilities, allow_software_fallback);
            }
            return make_software_fallback_plan(decode, capabilities);
        }
        if (decode.device.kind == SsvDecodeDeviceKind::Cuda) {
            if (has_factory(capabilities, nvdec_factory(decode.device))) {
                return make_nvdec_plan(
                    decode, capabilities, allow_software_fallback);
            }
            return make_software_fallback_plan(decode, capabilities);
        }
        if (capabilities.has_gstreamer_element("vah264dec")
            && capabilities.has_gstreamer_element("vapostproc")) {
            return make_vaapi_plan(
                decode, capabilities, allow_software_fallback);
        }
        if (capabilities.has_gstreamer_element("nvh264dec")) {
            return make_nvdec_plan(
                decode, capabilities, allow_software_fallback);
        }
        return make_software_plan(
            decode, capabilities, allow_software_fallback);
    case SsvDecodeMode::Vaapi:
        return make_vaapi_plan(decode, capabilities, false);
    case SsvDecodeMode::Nvdec:
        return make_nvdec_plan(decode, capabilities, false);
    case SsvDecodeMode::Software:
        return make_software_plan(decode, capabilities, false);
    }
    fail_plan(
        SsvExitCode::InvalidConfiguration,
        "config",
        "unsupported decode mode");
}

std::vector<SsvDecodeFallbackDecision> resolve_decode_fallbacks(
    const SsvDecodeConfig &decode,
    const SsvDecodePlan &resolved)
{
    if (decode.mode != SsvDecodeMode::Auto)
        return {};

    const auto va_reason =
        "VA H.264 decoder or post-processor is unavailable";
    const auto nvdec_reason = "NVDEC H.264 decoder is unavailable";
    if (decode.device.kind == SsvDecodeDeviceKind::Drm) {
        if (resolved.backend == SsvDecodeBackend::Software) {
            return {{
                SsvDecodeBackend::Vaapi,
                SsvDecodeBackend::Software,
                va_reason,
            }};
        }
        return {};
    }
    if (decode.device.kind == SsvDecodeDeviceKind::Cuda) {
        if (resolved.backend == SsvDecodeBackend::Software) {
            return {{
                SsvDecodeBackend::Nvdec,
                SsvDecodeBackend::Software,
                nvdec_reason,
            }};
        }
        return {};
    }
    if (resolved.backend == SsvDecodeBackend::Vaapi)
        return {};
    if (resolved.backend == SsvDecodeBackend::Nvdec) {
        return {{
            SsvDecodeBackend::Vaapi,
            SsvDecodeBackend::Nvdec,
            va_reason,
        }};
    }
    return {
        {
            SsvDecodeBackend::Vaapi,
            SsvDecodeBackend::Nvdec,
            va_reason,
        },
        {
            SsvDecodeBackend::Nvdec,
            SsvDecodeBackend::Software,
            nvdec_reason,
        },
    };
}

void validate_decode_device(const SsvDecodeConfig &decode)
{
    const bool compatible = [&] {
        switch (decode.mode) {
        case SsvDecodeMode::Auto:
            return true;
        case SsvDecodeMode::Vaapi:
            return decode.device.kind != SsvDecodeDeviceKind::Cuda;
        case SsvDecodeMode::Nvdec:
            return decode.device.kind != SsvDecodeDeviceKind::Drm;
        case SsvDecodeMode::Software:
            return decode.device.kind == SsvDecodeDeviceKind::Auto;
        }
        return false;
    }();
    if (!compatible) {
        fail_plan(
            SsvExitCode::InvalidConfiguration,
            "config.sources[0].decode.device",
            "decode device selector is incompatible with decode mode");
    }
}

bool decode_exports_dmabuf(SsvDecodeBackend backend)
{
    return backend == SsvDecodeBackend::Vaapi;
}

std::optional<SsvResolvedDisplayBackend> resolve_display_backend(
    const SsvDisplayConfig &display,
    const SsvHardwareCapabilities &capabilities,
    SsvDecodeBackend decode_backend)
{
    if (!display.enabled)
        return std::nullopt;

    const bool gtk_gl_elements_available =
        capabilities.has_gstreamer_element("gtkglsink")
        && capabilities.has_gstreamer_element("glupload")
        && capabilities.has_gstreamer_element("glcolorconvert");
    const bool gtk_available =
        capabilities.has_gstreamer_element("gtksink");

    switch (display.backend) {
    case SsvDisplayBackend::Auto:
        if (gtk_gl_elements_available
            && decode_exports_dmabuf(decode_backend)) {
            return SsvResolvedDisplayBackend::GtkGlSink;
        }
        if (gtk_available)
            return SsvResolvedDisplayBackend::GtkSink;
        break;
    case SsvDisplayBackend::GtkGlSink:
        if (!decode_exports_dmabuf(decode_backend)) {
            fail_plan(
                SsvExitCode::CapabilityUnavailable,
                "capability.display",
                kGtkGlDmabufRequirement);
        }
        if (gtk_gl_elements_available)
            return SsvResolvedDisplayBackend::GtkGlSink;
        break;
    case SsvDisplayBackend::GtkSink:
        if (gtk_available)
            return SsvResolvedDisplayBackend::GtkSink;
        break;
    }
    fail_plan(
        SsvExitCode::CapabilityUnavailable,
        "capability.display",
        "required GTK display backend is unavailable");
}

std::vector<std::string> resolve_display_fallback_reasons(
    const SsvDisplayConfig &display,
    const SsvHardwareCapabilities &capabilities,
    SsvDecodeBackend decode_backend,
    std::optional<SsvResolvedDisplayBackend> resolved_backend)
{
    std::vector<std::string> reasons;
    if (!display.enabled || display.backend != SsvDisplayBackend::Auto
        || resolved_backend != SsvResolvedDisplayBackend::GtkSink) {
        return reasons;
    }
    if (!decode_exports_dmabuf(decode_backend)) {
        reasons.emplace_back(kGtkGlDmabufRequirement);
    }
    for (const char *factory : {
             "gtkglsink",
             "glupload",
             "glcolorconvert",
         }) {
        if (!capabilities.has_gstreamer_element(factory)) {
            reasons.emplace_back(
                "missing GStreamer element: " + std::string(factory));
        }
    }
    return reasons;
}

std::optional<SsvInferenceBackend> resolve_inference_backend(
    const SsvInferenceConfig &inference,
    const SsvHardwareCapabilities &capabilities)
{
    if (!inference.enabled)
        return std::nullopt;

    if (std::holds_alternative<SsvOnnxRuntimeConfig>(inference.runtime)) {
        if (capabilities.onnxruntime_available)
            return SsvInferenceBackend::OnnxRuntime;
        fail_plan(
            SsvExitCode::ModelInitializationFailed,
            "inference.resolve",
            "ONNX Runtime is unavailable");
    }
    if (capabilities.tensorrt_engine_available)
        return SsvInferenceBackend::TensorRtEngine;
    fail_plan(
        SsvExitCode::ModelInitializationFailed,
        "inference.resolve",
        "TensorRT engine runtime is unavailable");
}

std::optional<SsvTrackingPlan> resolve_tracking_plan(
    const SsvConfig &config)
{
    if (!config.tracking.enabled)
        return std::nullopt;
    return SsvTrackingPlan {
        .nominal_frame_rate = config.inference.analysis_fps > 0
            ? std::optional<int> {config.inference.analysis_fps}
            : std::nullopt,
    };
}

SsvPipelineExpectedCaps resolve_expected_caps(
    SsvDecodeBackend decode_backend,
    std::optional<SsvResolvedDisplayBackend> display_backend,
    std::optional<SsvInferenceBackend> inference_backend)
{
    const bool vaapi = decode_backend == SsvDecodeBackend::Vaapi;
    SsvPipelineExpectedCaps caps {
        .decode_output = {
            SsvPixelFormat::Nv12,
            vaapi ? SsvMemoryKind::VaMemory
                  : SsvMemoryKind::SystemMemory,
        },
        .display_upload_input = std::nullopt,
        .display_sink_input = std::nullopt,
        .analysis_gpu_input = std::nullopt,
        .analysis_host_input = std::nullopt,
    };

    if (display_backend == SsvResolvedDisplayBackend::GtkGlSink) {
        caps.display_upload_input = SsvVideoCaps {
            SsvPixelFormat::Nv12,
            SsvMemoryKind::DmaBuf,
        };
        caps.display_sink_input = SsvVideoCaps {
            SsvPixelFormat::Rgba,
            SsvMemoryKind::GlMemory,
        };
    } else if (display_backend == SsvResolvedDisplayBackend::GtkSink) {
        caps.display_sink_input = SsvVideoCaps {
            SsvPixelFormat::Bgrx,
            SsvMemoryKind::SystemMemory,
        };
    }

    if (inference_backend) {
        if (vaapi) {
            caps.analysis_gpu_input = SsvVideoCaps {
                SsvPixelFormat::Nv12,
                SsvMemoryKind::VaMemory,
            };
        }
        caps.analysis_host_input = SsvVideoCaps {
            SsvPixelFormat::Rgba,
            SsvMemoryKind::SystemMemory,
        };
    }
    return caps;
}

} // namespace

SsvPipelinePlanError::SsvPipelinePlanError(
    SsvExitCode exit_code,
    std::string stage,
    std::string message)
    : std::runtime_error(std::move(message))
    , exit_code_(exit_code)
    , stage_(std::move(stage))
{
}

SsvExitCode SsvPipelinePlanError::exit_code() const noexcept
{
    return exit_code_;
}

const std::string &SsvPipelinePlanError::stage() const noexcept
{
    return stage_;
}

SsvPipelinePlan SsvPipelinePlan::resolve(
    const SsvConfig &config,
    const SsvHardwareCapabilities &capabilities)
{
    if (config.sources.size() != 1 || is_blank(config.sources.front().id)) {
        fail_plan(
            SsvExitCode::InvalidConfiguration,
            "config",
            "pipeline plan requires exactly one source with a non-empty id");
    }

    validate_decode_device(config.sources.front().decode);

    const auto decode = resolve_decode_plan(
        config.sources.front().decode, capabilities);
    const auto display_backend =
        resolve_display_backend(config.display, capabilities, decode.backend);
    const auto inference_backend =
        resolve_inference_backend(config.inference, capabilities);

    return {
        .source_id = config.sources.front().id,
        .decode = decode,
        .decode_fallbacks = resolve_decode_fallbacks(
            config.sources.front().decode, decode),
        .display_backend = display_backend,
        .display_fallback_allowed = config.display.enabled
            && config.display.backend == SsvDisplayBackend::Auto
            && display_backend == SsvResolvedDisplayBackend::GtkGlSink
            && capabilities.has_gstreamer_element("gtksink"),
        .display_fallback_reasons = resolve_display_fallback_reasons(
            config.display, capabilities, decode.backend, display_backend),
        .inference_backend = inference_backend,
        .tracking = resolve_tracking_plan(config),
        .expected_caps = resolve_expected_caps(
            decode.backend, display_backend, inference_backend),
    };
}

} // namespace ssv
