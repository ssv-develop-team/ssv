#include "ssv_inference_service.hpp"
#include "ssv_inference_test_service.hpp"
#include "ssv_meta.hpp"

#include <gst/check/gstcheck.h>
#include <gst/gst.h>
#include <gst/app/app.h>

#include <cstdlib>
#include <string>

GST_START_TEST(test_ssvinfer_uses_injected_service_for_rgba_input) {
    const char *source_id = "plugin-service-test";
    const char *label_map = std::getenv("SSV_TEST_LABEL_MAP_PATH");
    fail_unless(label_map != nullptr && label_map[0] != '\0');
    ssv::SsvInferenceConfig config;
    config.model.path = "/bin/true";
    config.model.output_format = "yolo_nx6";
    config.model.label_map = label_map;
    config.target_class = "person";
    auto service = ssv::infer::ssv_inference_test_service_create(config);
    ssv::infer::ssv_inference_service_update_source_geometry(
        service.get(), source_id, 6, 4);

    GError *error = nullptr;
    GstElement *pipeline = gst_parse_launch(
        "appsrc name=src format=time is-live=false block=true ! "
        "video/x-raw,format=RGBA,width=3,height=2,framerate=15/1 ! "
        "ssvinfer name=infer source-id=plugin-service-test ! "
        "fakesink sync=false",
        &error);
    fail_unless(error == nullptr);
    fail_unless(pipeline != nullptr);
    GstElement *src = gst_bin_get_by_name(GST_BIN(pipeline), "src");
    GstElement *infer = gst_bin_get_by_name(GST_BIN(pipeline), "infer");
    fail_unless(src != nullptr);
    fail_unless(infer != nullptr);
    g_object_set(infer, "inference-service", service.get(), nullptr);

    fail_unless(gst_element_set_state(pipeline, GST_STATE_PLAYING)
        != GST_STATE_CHANGE_FAILURE);
    GstBuffer *buffer = gst_buffer_new_allocate(nullptr, 24, nullptr);
    fail_unless(buffer != nullptr);
    GST_BUFFER_PTS(buffer) = GST_SECOND;
    GST_BUFFER_DURATION(buffer) = GST_SECOND / 15;
    fail_unless(gst_app_src_push_buffer(GST_APP_SRC(src), buffer)
        == GST_FLOW_OK);
    gst_app_src_end_of_stream(GST_APP_SRC(src));

    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *message = gst_bus_timed_pop_filtered(
        bus, 5 * GST_SECOND,
        static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
    fail_unless(message != nullptr);
    fail_unless(GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS);

    auto consumed = ssv_meta(source_id)->consume_detection();
    fail_unless(consumed.result == SsvMetaResult::Consumed);
    fail_unless(consumed.frame.has_value());
    fail_unless(consumed.frame->source_id == source_id);
    fail_unless(consumed.frame->timing.pts == GST_SECOND);
    fail_unless(consumed.frame->detections.size() == 1);
    fail_unless(consumed.frame->analysis_frame != nullptr);
    fail_unless(consumed.frame->analysis_frame->view().width == 3);
    fail_unless(consumed.frame->analysis_frame->view().height == 2);
    fail_unless(
        consumed.frame->analysis_frame->transform().source_width == 6);
    fail_unless(
        consumed.frame->analysis_frame->transform().source_height == 4);
    fail_unless(
        consumed.frame->analysis_frame->transform().scale == 0.5F);
    consumed.frame.reset();
    const auto analysis_stats =
        ssv::infer::ssv_inference_service_stats(service.get())
            .analysis_frames;
    fail_unless(analysis_stats.map_count == 1);
    fail_unless(analysis_stats.active_maps == 0);
    fail_unless(analysis_stats.outstanding_staging_leases == 0);

    gst_message_unref(message);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(src);
    gst_object_unref(infer);
    gst_object_unref(pipeline);
    ssv::infer::ssv_inference_service_stop(service.get());
}
GST_END_TEST

GST_START_TEST(test_ssvinfer_releases_analysis_frame_on_stop_without_tracker) {
    const char *source_id = "plugin-no-tracker-test";
    const char *label_map = std::getenv("SSV_TEST_LABEL_MAP_PATH");
    fail_unless(label_map != nullptr && label_map[0] != '\0');
    ssv::SsvInferenceConfig config;
    config.model.path = "/bin/true";
    config.model.output_format = "yolo_nx6";
    config.model.label_map = label_map;
    config.target_class = "person";
    auto service = ssv::infer::ssv_inference_test_service_create(config);
    ssv::infer::ssv_inference_service_update_source_geometry(
        service.get(), source_id, 3, 2);

    GError *error = nullptr;
    GstElement *pipeline = gst_parse_launch(
        "appsrc name=src format=time is-live=false block=true ! "
        "video/x-raw,format=RGBA,width=3,height=2,framerate=15/1 ! "
        "ssvinfer name=infer source-id=plugin-no-tracker-test ! "
        "fakesink sync=false",
        &error);
    fail_unless(error == nullptr);
    fail_unless(pipeline != nullptr);
    GstElement *src = gst_bin_get_by_name(GST_BIN(pipeline), "src");
    GstElement *infer = gst_bin_get_by_name(GST_BIN(pipeline), "infer");
    fail_unless(src != nullptr);
    fail_unless(infer != nullptr);
    g_object_set(infer, "inference-service", service.get(), nullptr);
    fail_unless(gst_element_set_state(pipeline, GST_STATE_PLAYING)
        != GST_STATE_CHANGE_FAILURE);

    GstBuffer *buffer = gst_buffer_new_allocate(nullptr, 24, nullptr);
    fail_unless(buffer != nullptr);
    GST_BUFFER_PTS(buffer) = GST_SECOND;
    GST_BUFFER_DURATION(buffer) = GST_SECOND / 15;
    fail_unless(gst_app_src_push_buffer(GST_APP_SRC(src), buffer)
        == GST_FLOW_OK);
    gst_app_src_end_of_stream(GST_APP_SRC(src));

    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *message = gst_bus_timed_pop_filtered(
        bus, 5 * GST_SECOND,
        static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
    fail_unless(message != nullptr);
    fail_unless(GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS);
    fail_unless(ssv::infer::ssv_inference_service_stats(service.get())
        .analysis_frames.active_maps == 1);

    gst_element_set_state(pipeline, GST_STATE_NULL);
    fail_unless(ssv_meta(source_id)->consume_detection().result
        == SsvMetaResult::Empty);
    const auto released =
        ssv::infer::ssv_inference_service_stats(service.get())
            .analysis_frames;
    fail_unless(released.active_maps == 0);
    fail_unless(released.outstanding_staging_leases == 0);

    gst_message_unref(message);
    gst_object_unref(bus);
    gst_object_unref(src);
    gst_object_unref(infer);
    gst_object_unref(pipeline);
    ssv::infer::ssv_inference_service_stop(service.get());
}
GST_END_TEST

GST_START_TEST(test_ssvinfer_holds_typed_service_reference) {
    GstElement *element = gst_element_factory_make("ssvinfer", nullptr);
    fail_unless(element != nullptr);

    auto *service = SSV_INFERENCE_SERVICE(
        g_object_new(SSV_TYPE_INFERENCE_SERVICE, nullptr));
    fail_unless(service != nullptr);
    g_object_set(element, "inference-service", service, nullptr);

    SsvInferenceService *observed = nullptr;
    g_object_get(element, "inference-service", &observed, nullptr);
    fail_unless(observed == service);

    g_object_unref(observed);
    g_object_unref(service);
    gst_object_unref(element);
}
GST_END_TEST

GST_START_TEST(test_ssvinfer_exposes_inference_service_property) {
    GstElement *element = gst_element_factory_make("ssvinfer", nullptr);
    fail_unless(element != nullptr);

    GObjectClass *object_class = G_OBJECT_GET_CLASS(element);
    GParamSpec *service_property =
        g_object_class_find_property(object_class, "inference-service");
    fail_unless(service_property != nullptr);
    fail_unless(
        G_PARAM_SPEC_VALUE_TYPE(service_property)
        == SSV_TYPE_INFERENCE_SERVICE);

    const char *legacy_properties[] = {
        "runtime",
        "model-path",
        "device",
        "device-id",
        "precision",
        "model-family",
        "output-format",
        "conf-threshold",
        "target-class",
        "label-map",
        "async",
        "cuda-device-id",
        "cuda-required",
    };
    for (const char *property : legacy_properties) {
        fail_unless(
            g_object_class_find_property(object_class, property) == nullptr,
            "legacy ssvinfer property is still exposed: %s",
            property);
    }

    gst_object_unref(element);
}
GST_END_TEST

int main(int argc, char **argv)
{
    gst_check_init(&argc, &argv);

    Suite *suite = suite_create("ssv-infer-plugin");
    TCase *tc = tcase_create("ssvinfer");
    tcase_add_test(tc, test_ssvinfer_uses_injected_service_for_rgba_input);
    tcase_add_test(
        tc, test_ssvinfer_releases_analysis_frame_on_stop_without_tracker);
    tcase_add_test(tc, test_ssvinfer_holds_typed_service_reference);
    tcase_add_test(tc, test_ssvinfer_exposes_inference_service_property);
    suite_add_tcase(suite, tc);

    SRunner *runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    const int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed == 0 ? 0 : 1;
}
