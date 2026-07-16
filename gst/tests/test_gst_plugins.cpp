#include "ssv_meta.hpp"
#include "ssv_preprocessor.hpp"
#include "ssv_yolo_parser.hpp"

#include <gst/check/gstcheck.h>
#include <gst/gst.h>
#include <gst/app/app.h>
#include <gst/video/video.h>

#include <cstring>
#include <cmath>
#include <string>

extern void run_ssv_meta_tests();

static void assert_element_factory(const char *name) {
    GstElement *element = gst_element_factory_make(name, nullptr);
    fail_unless(element != nullptr, "missing element factory: %s", name);
    gst_object_unref(element);
}

GST_START_TEST(test_ssv_plugin_factories_are_registered) {
    assert_element_factory("ssvtemplate");
    assert_element_factory("ssvinfer");
    assert_element_factory("ssvtrack");
    assert_element_factory("ssvpub");
    assert_element_factory("ssvoverlay");
}
GST_END_TEST

GST_START_TEST(test_ssvinfer_exposes_label_map_property) {
    GstElement *element = gst_element_factory_make("ssvinfer", nullptr);
    fail_unless(element != nullptr);

    gchar *target_class = nullptr;
    g_object_get(element, "target-class", &target_class, nullptr);
    fail_unless(target_class != nullptr);
    fail_unless(std::string(target_class).empty());
    g_free(target_class);

    g_object_set(element, "label-map", "config/model-labels/coco80.txt", nullptr);
    gchar *label_map = nullptr;
    g_object_get(element, "label-map", &label_map, nullptr);
    fail_unless(label_map != nullptr);
    fail_unless(std::string(label_map) == "config/model-labels/coco80.txt");

    g_free(label_map);
    gst_object_unref(element);
}
GST_END_TEST

GST_START_TEST(test_ssvinfer_exposes_runtime_properties) {
    GstElement *element = gst_element_factory_make("ssvinfer", nullptr);
    fail_unless(element != nullptr);

    gchar *runtime = nullptr;
    gchar *device = nullptr;
    gchar *precision = nullptr;
    gchar *model_family = nullptr;
    gchar *output_format = nullptr;
    gint device_id = -1;
    g_object_get(element,
        "runtime", &runtime,
        "device", &device,
        "device-id", &device_id,
        "precision", &precision,
        "model-family", &model_family,
        "output-format", &output_format,
        nullptr);
    fail_unless(runtime != nullptr);
    fail_unless(std::string(runtime) == "auto");
    fail_unless(device != nullptr);
    fail_unless(std::string(device) == "auto");
    fail_unless(device_id == 0);
    fail_unless(precision != nullptr);
    fail_unless(std::string(precision) == "auto");
    fail_unless(model_family != nullptr);
    fail_unless(std::string(model_family) == "yolo");
    fail_unless(output_format != nullptr);
    fail_unless(std::string(output_format) == "auto");
    g_free(runtime);
    g_free(device);
    g_free(precision);
    g_free(model_family);
    g_free(output_format);

    g_object_set(element,
        "runtime", "onnxruntime",
        "device", "gpu",
        "device-id", 1,
        "precision", "fp32",
        "model-family", "yolo",
        "output-format", "yolov8",
        nullptr);
    g_object_get(element,
        "runtime", &runtime,
        "device", &device,
        "device-id", &device_id,
        "precision", &precision,
        "model-family", &model_family,
        "output-format", &output_format,
        nullptr);
    fail_unless(runtime != nullptr);
    fail_unless(std::string(runtime) == "onnxruntime");
    fail_unless(device != nullptr);
    fail_unless(std::string(device) == "gpu");
    fail_unless(device_id == 1);
    fail_unless(precision != nullptr);
    fail_unless(std::string(precision) == "fp32");
    fail_unless(model_family != nullptr);
    fail_unless(std::string(model_family) == "yolo");
    fail_unless(output_format != nullptr);
    fail_unless(std::string(output_format) == "yolov8");

    fail_unless(g_object_class_find_property(G_OBJECT_GET_CLASS(element), "cuda-device-id") == nullptr);
    fail_unless(g_object_class_find_property(G_OBJECT_GET_CLASS(element), "cuda-required") == nullptr);

    g_free(runtime);
    g_free(device);
    g_free(precision);
    g_free(model_family);
    g_free(output_format);
    gst_object_unref(element);
}
GST_END_TEST

GST_START_TEST(test_ssvinfer_preprocessor_outputs_nchw_rgb_tensor) {
    ssv::infer::SsvVideoFrame frame;
    frame.frame_id = 1;
    frame.source_id = "unit-test";
    frame.width = 1;
    frame.height = 1;
    frame.stride = 3;
    frame.bgr = {10, 20, 30};

    ssv::infer::TensorSpec input;
    input.name = "images";
    input.shape = {1, 3, 1, 1};
    input.layout = ssv::infer::TensorLayout::Nchw;

    ssv::infer::Preprocessor preprocessor;
    auto result = preprocessor.run(frame, input);

    fail_unless(result.input.host_data.size() == 3);
    fail_unless(std::fabs(result.input.host_data[0] - (30.0f / 255.0f)) < 0.0001f);
    fail_unless(std::fabs(result.input.host_data[1] - (20.0f / 255.0f)) < 0.0001f);
    fail_unless(std::fabs(result.input.host_data[2] - (10.0f / 255.0f)) < 0.0001f);
}
GST_END_TEST

GST_START_TEST(test_ssvinfer_yolo_parser_parses_nx6_output) {
    ssv::infer::InferenceConfig config;
    config.output_format = ssv::infer::OutputFormat::YoloNx6;
    config.confidence_threshold = 0.5f;
    config.target_class = "head";

    ssv::infer::ModelMetadata metadata;
    ssv::infer::TensorSpec output_spec;
    output_spec.name = "output0";
    output_spec.shape = {1, 2, 6};
    metadata.outputs.push_back(output_spec);

    ssv::infer::YoloOutputParser parser;
    parser.configure(config, metadata, {"helmet", "head"});

    ssv::infer::Tensor output;
    output.spec = output_spec;
    output.host_data = {
        0.1f, 0.2f, 0.4f, 0.8f, 0.9f, 1.0f,
        0.2f, 0.2f, 0.5f, 0.8f, 0.4f, 1.0f,
    };

    ssv::infer::PreprocessResult preprocess;
    preprocess.original_width = 640;
    preprocess.original_height = 640;
    preprocess.model_width = 640;
    preprocess.model_height = 640;

    auto detections = parser.parse({output}, preprocess);

    fail_unless(detections.size() == 1);
    fail_unless(detections[0].class_id == 1);
    fail_unless(std::string(detections[0].class_name) == "head");
    fail_unless(std::fabs(detections[0].confidence - 0.9f) < 0.0001f);
}
GST_END_TEST

GST_START_TEST(test_ssvtrack_exposes_botsort_properties) {
    GstElement *element = gst_element_factory_make("ssvtrack", nullptr);
    fail_unless(element != nullptr);

    gfloat match_thresh = 0.0f;
    g_object_get(element, "match-thresh", &match_thresh, nullptr);
    fail_unless(match_thresh == 0.8f);

    fail_unless(g_object_class_find_property(G_OBJECT_GET_CLASS(element), "track-low-thresh") != nullptr);
    fail_unless(g_object_class_find_property(G_OBJECT_GET_CLASS(element), "track-high-thresh") != nullptr);
    fail_unless(g_object_class_find_property(G_OBJECT_GET_CLASS(element), "new-track-thresh") != nullptr);
    fail_unless(g_object_class_find_property(G_OBJECT_GET_CLASS(element), "gmc-method") != nullptr);
    fail_unless(g_object_class_find_property(G_OBJECT_GET_CLASS(element), "gmc-downscale") != nullptr);

    gchar *gmc_method = nullptr;
    g_object_get(element, "gmc-method", &gmc_method, nullptr);
    fail_unless(gmc_method != nullptr);
    fail_unless(std::string(gmc_method) == "sparse-opt-flow");
    g_free(gmc_method);

    g_object_set(element,
        "frame-rate", 25,
        "track-thresh", 0.55f,
        "track-buffer", 45,
        "match-thresh", 0.85f,
        "track-low-thresh", 0.15f,
        "track-high-thresh", 0.65f,
        "new-track-thresh", 0.75f,
        "gmc-method", "none",
        "gmc-downscale", 3,
        nullptr);

    gint frame_rate = 0;
    gfloat track_thresh = 0.0f;
    gint track_buffer = 0;
    gfloat configured_match_thresh = 0.0f;
    gfloat track_low_thresh = 0.0f;
    gfloat track_high_thresh = 0.0f;
    gfloat new_track_thresh = 0.0f;
    gint gmc_downscale = 0;
    g_object_get(element,
        "frame-rate", &frame_rate,
        "track-thresh", &track_thresh,
        "track-buffer", &track_buffer,
        "match-thresh", &configured_match_thresh,
        "track-low-thresh", &track_low_thresh,
        "track-high-thresh", &track_high_thresh,
        "new-track-thresh", &new_track_thresh,
        "gmc-method", &gmc_method,
        "gmc-downscale", &gmc_downscale,
        nullptr);
    fail_unless(frame_rate == 25);
    fail_unless(fabsf(track_thresh - 0.55f) < 0.0001f);
    fail_unless(track_buffer == 45);
    fail_unless(fabsf(configured_match_thresh - 0.85f) < 0.0001f);
    fail_unless(fabsf(track_low_thresh - 0.15f) < 0.0001f);
    fail_unless(fabsf(track_high_thresh - 0.65f) < 0.0001f);
    fail_unless(fabsf(new_track_thresh - 0.75f) < 0.0001f);
    fail_unless(gmc_method != nullptr);
    fail_unless(std::string(gmc_method) == "none");
    fail_unless(gmc_downscale == 3);
    g_free(gmc_method);

    gst_object_unref(element);
}
GST_END_TEST

GST_START_TEST(test_ssvoverlay_runs_on_rgb_buffer) {
    GstElement *pipeline = gst_parse_launch(
        "videotestsrc num-buffers=1 ! video/x-raw,format=RGB,width=64,height=48 ! "
        "ssvoverlay ! fakesink", nullptr);
    fail_unless(pipeline != nullptr);

    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    fail_unless(ret != GST_STATE_CHANGE_FAILURE);

    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *msg = gst_bus_timed_pop_filtered(
        bus, 5 * GST_SECOND,
        (GstMessageType)(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
    fail_unless(msg != nullptr);
    fail_unless(GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS);

    gst_message_unref(msg);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}
GST_END_TEST

GST_START_TEST(test_ssvoverlay_runs_on_bgrx_buffer) {
    GstElement *pipeline = gst_parse_launch(
        "videotestsrc num-buffers=1 ! video/x-raw,format=BGRx,width=64,height=48 ! "
        "ssvoverlay ! fakesink", nullptr);
    fail_unless(pipeline != nullptr);

    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    fail_unless(ret != GST_STATE_CHANGE_FAILURE);

    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *msg = gst_bus_timed_pop_filtered(
        bus, 5 * GST_SECOND,
        (GstMessageType)(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
    fail_unless(msg != nullptr);
    fail_unless(GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS);

    gst_message_unref(msg);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}
GST_END_TEST

GST_START_TEST(test_ssvoverlay_draws_latest_detection) {
    auto &store = SsvDetectionStore::instance();
    (void)store.take();
    (void)store.take_for_tracking();

    SsvFrameDetections det{};
    det.frame_id = 1;
    std::snprintf(det.source_id, sizeof(det.source_id), "unit-test");
    SsvDetection d{};
    std::snprintf(d.class_name, sizeof(d.class_name), "person");
    d.confidence = 0.9f;
    d.x1 = 0.1f;
    d.y1 = 0.1f;
    d.x2 = 0.4f;
    d.y2 = 0.4f;
    d.class_id = 0;
    det.detections.push_back(d);
    store.set_tracked(std::move(det));

    GstElement *pipeline = gst_parse_launch(
        "videotestsrc num-buffers=1 pattern=black ! video/x-raw,format=BGRx,width=64,height=48 ! "
        "ssvoverlay ! appsink name=sink sync=false emit-signals=false max-buffers=1 drop=false", nullptr);
    fail_unless(pipeline != nullptr);

    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    fail_unless(ret != GST_STATE_CHANGE_FAILURE);

    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    fail_unless(sink != nullptr);
    GstSample *sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
    fail_unless(sample != nullptr);

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstCaps *caps = gst_sample_get_caps(sample);
    GstVideoInfo info;
    gst_video_info_from_caps(&info, caps);
    GstVideoFrame frame;
    fail_unless(gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ));
    const uint8_t *px = static_cast<const uint8_t *>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
    int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
    int x = 6;
    int y = 10;
    const uint8_t *p = px + y * stride + x * 4;
    fail_unless(p[0] == 0 && p[1] == 255 && p[2] == 0, "overlay did not draw green pixel");
    gst_video_frame_unmap(&frame);

    gst_sample_unref(sample);
    gst_object_unref(sink);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}
GST_END_TEST

static Suite *ssv_gst_suite() {
    Suite *suite = suite_create("ssv-gst");
    TCase *tc = tcase_create("plugins");
    tcase_add_test(tc, test_ssv_plugin_factories_are_registered);
    tcase_add_test(tc, test_ssvinfer_exposes_label_map_property);
    tcase_add_test(tc, test_ssvinfer_exposes_runtime_properties);
    tcase_add_test(tc, test_ssvinfer_preprocessor_outputs_nchw_rgb_tensor);
    tcase_add_test(tc, test_ssvinfer_yolo_parser_parses_nx6_output);
    tcase_add_test(tc, test_ssvtrack_exposes_botsort_properties);
    tcase_add_test(tc, test_ssvoverlay_runs_on_rgb_buffer);
    tcase_add_test(tc, test_ssvoverlay_runs_on_bgrx_buffer);
    tcase_add_test(tc, test_ssvoverlay_draws_latest_detection);
    suite_add_tcase(suite, tc);
    return suite;
}

int main(int argc, char **argv) {
    gst_check_init(&argc, &argv);
    run_ssv_meta_tests();

    Suite *suite = ssv_gst_suite();
    SRunner *runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed == 0 ? 0 : 1;
}
