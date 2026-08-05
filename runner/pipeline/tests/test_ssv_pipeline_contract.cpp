#include "pipeline/ssv_hardware_capabilities.hpp"
#include "pipeline/ssv_pipeline_contract.hpp"
#include "pipeline/ssv_pipeline_contract_internal.hpp"
#include "pipeline/ssv_pipeline_plan.hpp"

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include <cassert>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

void mark_finalized(gpointer data, GObject *)
{
    *static_cast<bool *>(data) = true;
}

ssv::SsvPipelineContractExpectation expectation(
    ssv::SsvPipelineBoundary boundary,
    ssv::SsvPixelFormat format,
    std::initializer_list<ssv::SsvMemoryKind> memories,
    int width = 0,
    int height = 0)
{
    return {
        boundary,
        format,
        std::vector<ssv::SsvMemoryKind>(memories),
        width,
        height,
    };
}

ssv::SsvPipelineContractObservation observation(
    ssv::SsvPixelFormat format,
    ssv::SsvMemoryKind caps_memory,
    ssv::SsvMemoryKind buffer_memory,
    int width = 1920,
    int height = 1080)
{
    return {
        .format = format,
        .caps_memories = {caps_memory},
        .allocator_memory = std::nullopt,
        .buffer_memories = {buffer_memory},
        .width = width,
        .height = height,
    };
}

void assert_violation(
    const ssv::SsvPipelineContractExpectation &expected,
    const ssv::SsvPipelineContractObservation &actual,
    const std::string &fragment)
{
    const auto violation =
        ssv::ssv_pipeline_contract_validate(expected, actual);
    assert(violation.has_value());
    assert(violation->boundary == expected.boundary);
    assert(violation->message.find(fragment) != std::string::npos);
    assert(!violation->expected_caps.empty());
    assert(!violation->expected_allocator.empty());
    assert(!violation->expected_memory.empty());
    assert(!violation->actual_caps.empty());
    assert(!violation->actual_allocator.empty());
    assert(!violation->actual_memory.empty());
}

void test_rejects_accelerated_memory_regressions_without_mapping_pixels()
{
    const auto decode = expectation(
        ssv::SsvPipelineBoundary::DecodeTee,
        ssv::SsvPixelFormat::Nv12,
        {ssv::SsvMemoryKind::VaMemory, ssv::SsvMemoryKind::DmaBuf});
    assert_violation(
        decode,
        observation(
            ssv::SsvPixelFormat::Nv12,
            ssv::SsvMemoryKind::SystemMemory,
            ssv::SsvMemoryKind::SystemMemory),
        "caps memory");

    const auto display = expectation(
        ssv::SsvPipelineBoundary::DisplayUpload,
        ssv::SsvPixelFormat::Nv12,
        {ssv::SsvMemoryKind::DmaBuf});
    assert_violation(
        display,
        observation(
            ssv::SsvPixelFormat::Nv12,
            ssv::SsvMemoryKind::VaMemory,
            ssv::SsvMemoryKind::VaMemory),
        "caps memory");

    const auto analysis_gpu = expectation(
        ssv::SsvPipelineBoundary::AnalysisGpuInput,
        ssv::SsvPixelFormat::Nv12,
        {ssv::SsvMemoryKind::VaMemory, ssv::SsvMemoryKind::DmaBuf});
    assert_violation(
        analysis_gpu,
        observation(
            ssv::SsvPixelFormat::Nv12,
            ssv::SsvMemoryKind::SystemMemory,
            ssv::SsvMemoryKind::SystemMemory),
        "caps memory");

    const auto display_sink = expectation(
        ssv::SsvPipelineBoundary::DisplaySink,
        ssv::SsvPixelFormat::Rgba,
        {ssv::SsvMemoryKind::GlMemory});
    assert_violation(
        display_sink,
        observation(
            ssv::SsvPixelFormat::Rgba,
            ssv::SsvMemoryKind::SystemMemory,
            ssv::SsvMemoryKind::SystemMemory),
        "caps memory");
}

void test_requires_model_sized_rgba_system_memory_at_host_boundary()
{
    const auto host = expectation(
        ssv::SsvPipelineBoundary::AnalysisHost,
        ssv::SsvPixelFormat::Rgba,
        {ssv::SsvMemoryKind::SystemMemory},
        640,
        640);
    assert_violation(
        host,
        observation(
            ssv::SsvPixelFormat::Rgba,
            ssv::SsvMemoryKind::SystemMemory,
            ssv::SsvMemoryKind::SystemMemory,
            1280,
            720),
        "dimensions");

    auto wrong_allocator = observation(
        ssv::SsvPixelFormat::Rgba,
        ssv::SsvMemoryKind::SystemMemory,
        ssv::SsvMemoryKind::SystemMemory,
        640,
        640);
    wrong_allocator.allocator_memory = ssv::SsvMemoryKind::VaMemory;
    assert_violation(host, wrong_allocator, "allocator memory");

    const auto valid = ssv::ssv_pipeline_contract_validate(
        host,
        observation(
            ssv::SsvPixelFormat::Rgba,
            ssv::SsvMemoryKind::SystemMemory,
            ssv::SsvMemoryKind::SystemMemory,
            640,
            640));
    assert(!valid.has_value());
}

void test_rejects_incomplete_contract_observations()
{
    const auto decode = expectation(
        ssv::SsvPipelineBoundary::DecodeTee,
        ssv::SsvPixelFormat::Nv12,
        {ssv::SsvMemoryKind::VaMemory, ssv::SsvMemoryKind::DmaBuf});

    auto missing_format = observation(
        ssv::SsvPixelFormat::Nv12,
        ssv::SsvMemoryKind::VaMemory,
        ssv::SsvMemoryKind::VaMemory);
    missing_format.format = std::nullopt;
    assert_violation(decode, missing_format, "pixel format");

    auto missing_caps = observation(
        ssv::SsvPixelFormat::Nv12,
        ssv::SsvMemoryKind::VaMemory,
        ssv::SsvMemoryKind::VaMemory);
    missing_caps.caps_memories.clear();
    assert_violation(decode, missing_caps, "caps memory");

    const auto missing_allocator = observation(
        ssv::SsvPixelFormat::Nv12,
        ssv::SsvMemoryKind::VaMemory,
        ssv::SsvMemoryKind::VaMemory);
    assert_violation(decode, missing_allocator, "allocator memory");

    const auto host = expectation(
        ssv::SsvPipelineBoundary::AnalysisHost,
        ssv::SsvPixelFormat::Rgba,
        {ssv::SsvMemoryKind::SystemMemory},
        640,
        640);
    auto missing_dimensions = observation(
        ssv::SsvPixelFormat::Rgba,
        ssv::SsvMemoryKind::SystemMemory,
        ssv::SsvMemoryKind::SystemMemory);
    missing_dimensions.width = 0;
    missing_dimensions.height = 0;
    assert_violation(host, missing_dimensions, "dimensions");
}

ssv::SsvPipelinePlan make_plan(bool fallback_allowed)
{
    ssv::SsvPipelinePlan plan;
    plan.decode = {
        .backend = ssv::SsvDecodeBackend::Vaapi,
        .device = {},
        .decoder_factory = "vah264dec",
        .va_postproc_factory = "vapostproc",
        .software_fallback_allowed = fallback_allowed,
    };
    return plan;
}

void test_only_auto_decode_requests_software_recovery()
{
    assert(ssv::ssv_pipeline_contract_recovery(
               make_plan(true))
        == ssv::SsvPipelineContractRecovery::FallbackSoftware);
    assert(ssv::ssv_pipeline_contract_recovery(
               make_plan(false))
        == ssv::SsvPipelineContractRecovery::Fatal);
    assert(ssv::ssv_pipeline_contract_error_quark() != 0);
}

void test_gst_probe_reports_memory_type_without_mapping_the_frame()
{
    GError *parse_error = nullptr;
    GstElement *pipeline = gst_parse_launch(
        "appsrc name=source format=time is-live=false "
        "caps=\"video/x-raw(memory:VAMemory),format=NV12,width=16,height=16,framerate=1/1\" ! "
        "identity name=boundary ! fakesink sync=false",
        &parse_error);
    assert(parse_error == nullptr);
    assert(pipeline != nullptr);
    GstElement *source = gst_bin_get_by_name(GST_BIN(pipeline), "source");
    GstElement *boundary = gst_bin_get_by_name(GST_BIN(pipeline), "boundary");
    GstPad *pad = gst_element_get_static_pad(boundary, "src");
    assert(source != nullptr && boundary != nullptr && pad != nullptr);
    ssv::pipeline_internal::watch_contract(
        pipeline,
        pad,
        expectation(
            ssv::SsvPipelineBoundary::DecodeTee,
            ssv::SsvPixelFormat::Nv12,
            {ssv::SsvMemoryKind::VaMemory}));
    gst_object_unref(pad);

    assert(gst_element_set_state(pipeline, GST_STATE_PLAYING)
        != GST_STATE_CHANGE_FAILURE);
    GstBuffer *buffer = gst_buffer_new_allocate(nullptr, 16 * 16 * 3 / 2, nullptr);
    assert(buffer != nullptr);
    GST_BUFFER_PTS(buffer) = 0;
    GST_BUFFER_DURATION(buffer) = GST_SECOND;
    static_cast<void>(gst_app_src_push_buffer(GST_APP_SRC(source), buffer));

    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *message = gst_bus_timed_pop_filtered(
        bus, 5 * GST_SECOND, GST_MESSAGE_ERROR);
    assert(message != nullptr);
    GError *contract_error = nullptr;
    gchar *debug = nullptr;
    gst_message_parse_error(message, &contract_error, &debug);
    assert(contract_error != nullptr);
    assert(contract_error->domain == ssv::ssv_pipeline_contract_error_quark());
    const auto violation =
        ssv::ssv_pipeline_contract_violation_from_message(message);
    assert(violation.has_value());
    assert(violation->boundary == ssv::SsvPipelineBoundary::DecodeTee);
    assert(violation->expected_caps.find("memory:VAMemory")
        != std::string::npos);
    assert(violation->expected_allocator == "VAMemory");
    assert(violation->expected_memory == "VAMemory");
    assert(violation->actual_caps.find("memory:VAMemory")
        != std::string::npos);
    assert(violation->actual_allocator == "unobserved");
    assert(violation->actual_memory == "SystemMemory");

    g_clear_error(&contract_error);
    g_free(debug);
    gst_message_unref(message);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(source);
    gst_object_unref(boundary);
    gst_object_unref(pipeline);
}

void test_contract_watch_does_not_own_error_source()
{
    GstElement *pipeline = gst_pipeline_new("contract-lifetime-test");
    GstElement *boundary = gst_element_factory_make("identity", nullptr);
    assert(pipeline != nullptr && boundary != nullptr);
    assert(gst_bin_add(GST_BIN(pipeline), boundary));
    GstPad *pad = gst_element_get_static_pad(boundary, "src");
    assert(pad != nullptr);

    ssv::pipeline_internal::watch_contract(
        pipeline,
        pad,
        expectation(
            ssv::SsvPipelineBoundary::DecodeTee,
            ssv::SsvPixelFormat::Nv12,
            {ssv::SsvMemoryKind::VaMemory}));
    gst_object_unref(pad);

    bool pipeline_finalized = false;
    g_object_weak_ref(
        G_OBJECT(pipeline), mark_finalized, &pipeline_finalized);
    gst_object_unref(pipeline);
    assert(pipeline_finalized);
}

void test_gst_probe_ignores_non_memory_caps_features()
{
    GError *parse_error = nullptr;
    GstElement *pipeline = gst_parse_launch(
        "appsrc name=source format=time is-live=false "
        "caps=\"video/x-raw(memory:SystemMemory,meta:GstVideoMeta),format=NV12,width=16,height=16,framerate=1/1\" ! "
        "identity name=boundary ! fakesink sync=false",
        &parse_error);
    assert(parse_error == nullptr);
    assert(pipeline != nullptr);
    GstElement *source = gst_bin_get_by_name(GST_BIN(pipeline), "source");
    GstElement *boundary = gst_bin_get_by_name(GST_BIN(pipeline), "boundary");
    GstPad *pad = gst_element_get_static_pad(boundary, "src");
    assert(source != nullptr && boundary != nullptr && pad != nullptr);
    ssv::pipeline_internal::watch_contract(
        pipeline,
        pad,
        expectation(
            ssv::SsvPipelineBoundary::AnalysisHost,
            ssv::SsvPixelFormat::Nv12,
            {ssv::SsvMemoryKind::SystemMemory},
            16,
            16));
    gst_object_unref(pad);

    assert(gst_element_set_state(pipeline, GST_STATE_PLAYING)
        != GST_STATE_CHANGE_FAILURE);
    GstBuffer *buffer = gst_buffer_new_allocate(nullptr, 16 * 16 * 3 / 2, nullptr);
    assert(buffer != nullptr);
    GST_BUFFER_PTS(buffer) = 0;
    GST_BUFFER_DURATION(buffer) = GST_SECOND;
    assert(gst_app_src_push_buffer(GST_APP_SRC(source), buffer) == GST_FLOW_OK);
    assert(gst_app_src_end_of_stream(GST_APP_SRC(source)) == GST_FLOW_OK);

    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *message = gst_bus_timed_pop_filtered(
        bus,
        5 * GST_SECOND,
        static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
    assert(message != nullptr);
    assert(GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS);

    gst_message_unref(message);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(source);
    gst_object_unref(boundary);
    gst_object_unref(pipeline);
}

void test_gst_probe_reports_ready_once_after_complete_buffer()
{
    GError *parse_error = nullptr;
    GstElement *pipeline = gst_parse_launch(
        "appsrc name=source format=time is-live=false "
        "caps=\"video/x-raw,format=RGBA,width=2,height=2,framerate=1/1\" ! "
        "identity name=boundary ! fakesink sync=false",
        &parse_error);
    assert(parse_error == nullptr);
    assert(pipeline != nullptr);
    GstElement *source = gst_bin_get_by_name(GST_BIN(pipeline), "source");
    GstElement *boundary = gst_bin_get_by_name(GST_BIN(pipeline), "boundary");
    GstPad *pad = gst_element_get_static_pad(boundary, "src");
    assert(source != nullptr && boundary != nullptr && pad != nullptr);
    ssv::pipeline_internal::watch_contract(
        pipeline,
        pad,
        expectation(
            ssv::SsvPipelineBoundary::AnalysisHost,
            ssv::SsvPixelFormat::Rgba,
            {ssv::SsvMemoryKind::SystemMemory},
            2,
            2));
    gst_object_unref(pad);

    assert(gst_element_set_state(pipeline, GST_STATE_PLAYING)
        != GST_STATE_CHANGE_FAILURE);
    for (int index = 0; index < 2; ++index) {
        GstBuffer *buffer = gst_buffer_new_allocate(nullptr, 16, nullptr);
        assert(buffer != nullptr);
        GST_BUFFER_PTS(buffer) = index * GST_SECOND;
        GST_BUFFER_DURATION(buffer) = GST_SECOND;
        assert(gst_app_src_push_buffer(GST_APP_SRC(source), buffer)
            == GST_FLOW_OK);
    }
    assert(gst_app_src_end_of_stream(GST_APP_SRC(source)) == GST_FLOW_OK);

    GstBus *bus = gst_element_get_bus(pipeline);
    int ready_count = 0;
    bool eos = false;
    while (!eos) {
        GstMessage *message = gst_bus_timed_pop(bus, 5 * GST_SECOND);
        assert(message != nullptr);
        if (const auto ready =
                ssv::ssv_pipeline_contract_ready_from_message(message)) {
            assert(*ready == ssv::SsvPipelineBoundary::AnalysisHost);
            ++ready_count;
        } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            assert(false && "valid contract posted an error");
        } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
            eos = true;
        }
        gst_message_unref(message);
    }
    assert(ready_count == 1);

    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(source);
    gst_object_unref(boundary);
    gst_object_unref(pipeline);
}

} // namespace

int main(int argc, char **argv)
{
    gst_init(&argc, &argv);
    test_rejects_accelerated_memory_regressions_without_mapping_pixels();
    test_requires_model_sized_rgba_system_memory_at_host_boundary();
    test_rejects_incomplete_contract_observations();
    test_only_auto_decode_requests_software_recovery();
    test_gst_probe_reports_memory_type_without_mapping_the_frame();
    test_contract_watch_does_not_own_error_source();
    test_gst_probe_ignores_non_memory_caps_features();
    test_gst_probe_reports_ready_once_after_complete_buffer();
    return 0;
}
