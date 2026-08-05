#include "pipeline/ssv_hardware_capabilities.hpp"
#include "pipeline/ssv_pipeline_plan.hpp"

#include <cassert>
#include <initializer_list>
#include <string>
#include <utility>

namespace {

class FakeHardwareCapabilitiesProbe final
    : public ssv::SsvHardwareCapabilitiesProbe {
public:
    explicit FakeHardwareCapabilitiesProbe(
        ssv::SsvHardwareCapabilities capabilities)
        : capabilities_(std::move(capabilities))
    {
    }

    [[nodiscard]] ssv::SsvHardwareCapabilities detect() const override
    {
        return capabilities_;
    }

private:
    ssv::SsvHardwareCapabilities capabilities_;
};

ssv::SsvConfig make_config()
{
    ssv::SsvConfig config;
    ssv::SsvSourceConfig source;
    source.id = "camera-01";
    source.uri = "rtsp://127.0.0.1/test";
    config.sources.push_back(std::move(source));
    return config;
}

ssv::SsvHardwareCapabilities make_capabilities(
    std::initializer_list<const char *> elements,
    bool onnxruntime_available = true,
    bool tensorrt_engine_available = false)
{
    ssv::SsvHardwareCapabilities capabilities;
    for (const auto *element : elements)
        capabilities.gstreamer_elements.emplace_back(element);
    capabilities.onnxruntime_available = onnxruntime_available;
    capabilities.tensorrt_engine_available = tensorrt_engine_available;
    return capabilities;
}

void expect_plan_error(
    const ssv::SsvConfig &config,
    const ssv::SsvHardwareCapabilities &capabilities,
    ssv::SsvExitCode exit_code,
    const std::string &stage)
{
    try {
        static_cast<void>(
            ssv::SsvPipelinePlan::resolve(config, capabilities));
        assert(false && "pipeline plan unexpectedly resolved");
    } catch (const ssv::SsvPipelinePlanError &error) {
        assert(error.exit_code() == exit_code);
        assert(error.stage() == stage);
        assert(std::string(error.what()).empty() == false);
    }
}

void test_fake_probe_and_auto_resolution_prefer_acceleration()
{
    const FakeHardwareCapabilitiesProbe probe(make_capabilities({
        "vah264dec",
        "vapostproc",
        "nvh264dec",
        "avdec_h264",
        "gtkglsink",
        "gtksink",
        "glupload",
        "glcolorconvert",
    }));
    const auto capabilities = probe.detect();
    const auto plan =
        ssv::SsvPipelinePlan::resolve(make_config(), capabilities);

    assert(plan.source_id == "camera-01");
    assert(plan.decode.backend == ssv::SsvDecodeBackend::Vaapi);
    assert(plan.display_backend
        == ssv::SsvResolvedDisplayBackend::GtkGlSink);
    assert(plan.display_fallback_allowed);
    assert(plan.display_fallback_reasons.empty());
    assert(plan.inference_backend
        == ssv::SsvInferenceBackend::OnnxRuntime);
    assert((plan.expected_caps.decode_output
        == ssv::SsvVideoCaps {
            ssv::SsvPixelFormat::Nv12,
            ssv::SsvMemoryKind::VaMemory,
        }));
    assert((plan.expected_caps.display_upload_input
        == ssv::SsvVideoCaps {
            ssv::SsvPixelFormat::Nv12,
            ssv::SsvMemoryKind::DmaBuf,
        }));
    assert((plan.expected_caps.display_sink_input
        == ssv::SsvVideoCaps {
            ssv::SsvPixelFormat::Rgba,
            ssv::SsvMemoryKind::GlMemory,
        }));
    assert((plan.expected_caps.analysis_gpu_input
        == ssv::SsvVideoCaps {
            ssv::SsvPixelFormat::Nv12,
            ssv::SsvMemoryKind::VaMemory,
        }));
    assert((plan.expected_caps.analysis_host_input
        == ssv::SsvVideoCaps {
            ssv::SsvPixelFormat::Rgba,
            ssv::SsvMemoryKind::SystemMemory,
        }));
}

void test_auto_resolution_falls_back_in_declared_order()
{
    auto config = make_config();
    auto capabilities = make_capabilities({
        "nvh264dec",
        "avdec_h264",
        "gtksink",
    });
    auto plan = ssv::SsvPipelinePlan::resolve(config, capabilities);

    assert(plan.decode.backend == ssv::SsvDecodeBackend::Nvdec);
    assert(plan.decode_fallbacks.size() == 1);
    assert(plan.decode_fallbacks.front().from
        == ssv::SsvDecodeBackend::Vaapi);
    assert(plan.decode_fallbacks.front().to
        == ssv::SsvDecodeBackend::Nvdec);
    assert(!plan.decode_fallbacks.front().reason.empty());
    assert(plan.display_backend
        == ssv::SsvResolvedDisplayBackend::GtkSink);
    assert(!plan.display_fallback_allowed);
    assert(plan.display_fallback_reasons
        == std::vector<std::string>({
            "gtkglsink requires DMABuf input from the resolved decoder",
            "missing GStreamer element: gtkglsink",
            "missing GStreamer element: glupload",
            "missing GStreamer element: glcolorconvert",
        }));
    assert((plan.expected_caps.decode_output
        == ssv::SsvVideoCaps {
            ssv::SsvPixelFormat::Nv12,
            ssv::SsvMemoryKind::SystemMemory,
        }));
    assert(plan.expected_caps.display_upload_input == std::nullopt);
    assert((plan.expected_caps.display_sink_input
        == ssv::SsvVideoCaps {
            ssv::SsvPixelFormat::Bgrx,
            ssv::SsvMemoryKind::SystemMemory,
        }));

    capabilities.gstreamer_elements = {"avdec_h264", "gtksink"};
    plan = ssv::SsvPipelinePlan::resolve(config, capabilities);
    assert(plan.decode.backend == ssv::SsvDecodeBackend::Software);
    assert(plan.decode_fallbacks.size() == 2);
    assert(plan.decode_fallbacks[0].from
        == ssv::SsvDecodeBackend::Vaapi);
    assert(plan.decode_fallbacks[0].to
        == ssv::SsvDecodeBackend::Nvdec);
    assert(plan.decode_fallbacks[1].from
        == ssv::SsvDecodeBackend::Nvdec);
    assert(plan.decode_fallbacks[1].to
        == ssv::SsvDecodeBackend::Software);
}

void test_auto_display_uses_gtksink_without_decoder_dmabuf()
{
    auto config = make_config();
    config.sources.front().decode.mode = ssv::SsvDecodeMode::Software;
    const auto capabilities = make_capabilities({
        "avdec_h264",
        "gtkglsink",
        "glupload",
        "glcolorconvert",
        "gtksink",
    });

    const auto plan = ssv::SsvPipelinePlan::resolve(config, capabilities);

    assert(plan.decode.backend == ssv::SsvDecodeBackend::Software);
    assert(plan.display_backend
        == ssv::SsvResolvedDisplayBackend::GtkSink);
    assert(!plan.display_fallback_allowed);
    assert(plan.display_fallback_reasons
        == std::vector<std::string>({
            "gtkglsink requires DMABuf input from the resolved decoder",
        }));
    assert(plan.expected_caps.display_upload_input == std::nullopt);
    assert((plan.expected_caps.display_sink_input
        == ssv::SsvVideoCaps {
            ssv::SsvPixelFormat::Bgrx,
            ssv::SsvMemoryKind::SystemMemory,
        }));
}

void test_explicit_gtk_gl_requires_decoder_dmabuf()
{
    auto config = make_config();
    config.sources.front().decode.mode = ssv::SsvDecodeMode::Software;
    config.display.backend = ssv::SsvDisplayBackend::GtkGlSink;

    expect_plan_error(
        config,
        make_capabilities({
            "avdec_h264",
            "gtkglsink",
            "glupload",
            "glcolorconvert",
        }),
        ssv::SsvExitCode::CapabilityUnavailable,
        "capability.display");
}

void test_disabled_branches_do_not_require_capabilities()
{
    auto config = make_config();
    config.display.enabled = false;
    config.inference.enabled = false;
    const auto plan = ssv::SsvPipelinePlan::resolve(
        config, make_capabilities({"avdec_h264"}, false, false));

    assert(plan.display_backend == std::nullopt);
    assert(plan.inference_backend == std::nullopt);
    assert(plan.expected_caps.display_upload_input == std::nullopt);
    assert(plan.expected_caps.display_sink_input == std::nullopt);
    assert(plan.expected_caps.analysis_gpu_input == std::nullopt);
    assert(plan.expected_caps.analysis_host_input == std::nullopt);
}

void test_explicit_backends_are_strict()
{
    auto config = make_config();
    config.sources.front().decode.mode = ssv::SsvDecodeMode::Vaapi;
    expect_plan_error(
        config,
        make_capabilities({"nvh264dec", "avdec_h264", "gtksink"}),
        ssv::SsvExitCode::CapabilityUnavailable,
        "capability.decode");

    config = make_config();
    config.display.backend = ssv::SsvDisplayBackend::GtkGlSink;
    expect_plan_error(
        config,
        make_capabilities({
            "avdec_h264",
            "gtkglsink",
            "glupload",
        }),
        ssv::SsvExitCode::CapabilityUnavailable,
        "capability.display");

    config = make_config();
    expect_plan_error(
        config,
        make_capabilities({
            "avdec_h264",
            "gtksink",
        }, false),
        ssv::SsvExitCode::ModelInitializationFailed,
        "inference.resolve");
}

void test_decode_device_selector_must_match_the_requested_backend()
{
    auto config = make_config();
    config.sources.front().decode.mode = ssv::SsvDecodeMode::Vaapi;
    config.sources.front().decode.device = {
        ssv::SsvDecodeDeviceKind::Cuda,
        "2",
    };
    expect_plan_error(
        config,
        make_capabilities({"vah264dec", "vapostproc", "gtksink"}),
        ssv::SsvExitCode::InvalidConfiguration,
        "config.sources[0].decode.device");

    config = make_config();
    config.sources.front().decode.mode = ssv::SsvDecodeMode::Software;
    config.sources.front().decode.device = {
        ssv::SsvDecodeDeviceKind::Drm,
        "/dev/dri/renderD128",
    };
    expect_plan_error(
        config,
        make_capabilities({"avdec_h264", "gtksink"}),
        ssv::SsvExitCode::InvalidConfiguration,
        "config.sources[0].decode.device");
}

void test_decode_plan_resolves_exact_device_factories_and_fallback_policy()
{
    auto config = make_config();
    config.sources.front().decode.mode = ssv::SsvDecodeMode::Vaapi;
    config.sources.front().decode.device = {
        ssv::SsvDecodeDeviceKind::Drm,
        "/dev/dri/renderD129",
    };
    auto plan = ssv::SsvPipelinePlan::resolve(
        config,
        make_capabilities({
            "varenderD129h264dec",
            "varenderD129postproc",
            "gtksink",
        }));
    assert(plan.decode.backend == ssv::SsvDecodeBackend::Vaapi);
    assert(plan.decode.decoder_factory == "varenderD129h264dec");
    assert(plan.decode.va_postproc_factory == "varenderD129postproc");
    assert(plan.decode.device.value == "/dev/dri/renderD129");
    assert(!plan.decode.software_fallback_allowed);
    assert(plan.decode_fallbacks.empty());

    config = make_config();
    config.sources.front().decode.mode = ssv::SsvDecodeMode::Nvdec;
    config.sources.front().decode.device = {
        ssv::SsvDecodeDeviceKind::Cuda,
        "2",
    };
    plan = ssv::SsvPipelinePlan::resolve(
        config,
        make_capabilities({"nvh264device2dec", "gtksink"}));
    assert(plan.decode.backend == ssv::SsvDecodeBackend::Nvdec);
    assert(plan.decode.decoder_factory == "nvh264device2dec");
    assert(plan.decode.va_postproc_factory.empty());
    assert(!plan.decode.software_fallback_allowed);
    assert(plan.decode_fallbacks.empty());

    config = make_config();
    plan = ssv::SsvPipelinePlan::resolve(
        config,
        make_capabilities({"avdec_h264", "gtksink"}));
    assert(plan.decode.backend == ssv::SsvDecodeBackend::Software);
    assert(plan.decode.decoder_factory == "avdec_h264");
    assert(plan.decode.software_fallback_allowed);
}

void test_auto_mode_keeps_software_fallback_with_explicit_device()
{
    auto config = make_config();
    config.sources.front().decode.device = {
        ssv::SsvDecodeDeviceKind::Drm,
        "/dev/dri/renderD129",
    };
    auto plan = ssv::SsvPipelinePlan::resolve(
        config,
        make_capabilities({
            "varenderD129h264dec",
            "varenderD129postproc",
            "gtksink",
        }));
    assert(plan.decode.backend == ssv::SsvDecodeBackend::Vaapi);
    assert(plan.decode.software_fallback_allowed);

    config.sources.front().decode.device = {
        ssv::SsvDecodeDeviceKind::Cuda,
        "2",
    };
    plan = ssv::SsvPipelinePlan::resolve(
        config,
        make_capabilities({"nvh264device2dec", "gtksink"}));
    assert(plan.decode.backend == ssv::SsvDecodeBackend::Nvdec);
    assert(plan.decode.software_fallback_allowed);
}

void test_auto_explicit_device_uses_software_when_acceleration_is_unavailable()
{
    auto config = make_config();
    config.sources.front().decode.device = {
        ssv::SsvDecodeDeviceKind::Drm,
        "/dev/dri/renderD129",
    };
    auto plan = ssv::SsvPipelinePlan::resolve(
        config, make_capabilities({"avdec_h264", "gtksink"}));
    assert(plan.decode.backend == ssv::SsvDecodeBackend::Software);
    assert(plan.decode.device.kind == ssv::SsvDecodeDeviceKind::Auto);
    assert(plan.decode.software_fallback_allowed);

    config.sources.front().decode.device = {
        ssv::SsvDecodeDeviceKind::Cuda,
        "2",
    };
    plan = ssv::SsvPipelinePlan::resolve(
        config, make_capabilities({"avdec_h264", "gtksink"}));
    assert(plan.decode.backend == ssv::SsvDecodeBackend::Software);
    assert(plan.decode.device.kind == ssv::SsvDecodeDeviceKind::Auto);
    assert(plan.decode.software_fallback_allowed);
}

void test_tensorrt_engine_runtime_resolves_as_a_value()
{
    auto config = make_config();
    config.inference.runtime = ssv::SsvTensorRtEngineConfig {};
    const auto plan = ssv::SsvPipelinePlan::resolve(
        config,
        make_capabilities({"avdec_h264", "gtksink"}, true, true));

    assert(plan.inference_backend
        == ssv::SsvInferenceBackend::TensorRtEngine);
}

void test_tracking_uses_analysis_rate_as_nominal_frame_rate()
{
    auto config = make_config();
    config.inference.analysis_fps = 12;
    const auto plan = ssv::SsvPipelinePlan::resolve(
        config,
        make_capabilities({"avdec_h264", "gtksink"}));

    assert(plan.tracking.has_value());
    assert(plan.tracking->nominal_frame_rate == 12);
}

void test_unlimited_analysis_rate_defers_nominal_tracking_rate()
{
    auto config = make_config();
    config.inference.analysis_fps = 0;
    const auto plan = ssv::SsvPipelinePlan::resolve(
        config,
        make_capabilities({"avdec_h264", "gtksink"}));

    assert(plan.tracking.has_value());
    assert(plan.tracking->nominal_frame_rate == std::nullopt);
}

void test_disabled_tracking_has_no_tracking_plan()
{
    auto config = make_config();
    config.tracking.enabled = false;
    const auto plan = ssv::SsvPipelinePlan::resolve(
        config,
        make_capabilities({"avdec_h264", "gtksink"}));

    assert(plan.tracking == std::nullopt);
}

void test_plan_requires_one_non_empty_source_identity()
{
    auto config = make_config();
    config.sources.clear();
    expect_plan_error(
        config,
        make_capabilities({}),
        ssv::SsvExitCode::InvalidConfiguration,
        "config");

    config = make_config();
    config.sources.front().id = "   ";
    expect_plan_error(
        config,
        make_capabilities({}),
        ssv::SsvExitCode::InvalidConfiguration,
        "config");
}

} // namespace

int main()
{
    test_fake_probe_and_auto_resolution_prefer_acceleration();
    test_auto_resolution_falls_back_in_declared_order();
    test_auto_display_uses_gtksink_without_decoder_dmabuf();
    test_explicit_gtk_gl_requires_decoder_dmabuf();
    test_disabled_branches_do_not_require_capabilities();
    test_explicit_backends_are_strict();
    test_decode_device_selector_must_match_the_requested_backend();
    test_decode_plan_resolves_exact_device_factories_and_fallback_policy();
    test_auto_mode_keeps_software_fallback_with_explicit_device();
    test_auto_explicit_device_uses_software_when_acceleration_is_unavailable();
    test_tensorrt_engine_runtime_resolves_as_a_value();
    test_tracking_uses_analysis_rate_as_nominal_frame_rate();
    test_unlimited_analysis_rate_defers_nominal_tracking_rate();
    test_disabled_tracking_has_no_tracking_plan();
    test_plan_requires_one_non_empty_source_identity();
    return 0;
}
