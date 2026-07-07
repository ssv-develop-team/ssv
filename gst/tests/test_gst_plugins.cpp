#include "ssv_config.hpp"
#include "ssv_meta.hpp"
#include "../ssv-infer/provider/runtime.hpp"
#include "../ssv-infer/provider/onnx_gpu_runtime.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>

#include <gst/check/gstcheck.h>
#include <gst/gst.h>
#include <gst/app/app.h>
#include <gst/video/video.h>

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

GST_START_TEST(test_ssv_config_reads_runtime_and_device_options) {
    const char *path = "/tmp/ssv-runtime-config.yaml";
    std::ofstream out(path);
    out << "runtime:\n"
           "  backend: \"onnx\"\n"
           "  device: \"gpu\"\n"
           "inference:\n"
           "  model_path: \"models/yolov8n.onnx\"\n"
           "  confidence_threshold: 0.6\n";
    out.close();

    ssv::SsvConfig cfg = ssv::ssv_config_load(path);
    fail_unless(cfg.runtime_backend == "onnx");
    fail_unless(cfg.runtime_device == "gpu");
    fail_unless(cfg.confidence_threshold == 0.6f);

    std::remove(path);
}
GST_END_TEST

GST_START_TEST(test_runtime_factory_accepts_onnx_cpu) {
    std::string error;

    std::unique_ptr<ssv::provider::Runtime> runtime =
        ssv::provider::create_runtime("onnx", "cpu", &error);
    fail_unless(runtime != nullptr);
    fail_unless(error.empty());

    Ort::SessionOptions options;
    ssv::provider::RuntimeStatus status;
    fail_unless(runtime->configure(options, &status, &error));
    fail_unless(status.requested_runtime == "onnx");
    fail_unless(status.active_runtime == "CPUExecutionProvider");
}
GST_END_TEST

GST_START_TEST(test_runtime_factory_rejects_unknown_device) {
    std::string error;

    std::unique_ptr<ssv::provider::Runtime> runtime =
        ssv::provider::create_runtime("onnx", "auto", &error);
    fail_unless(runtime == nullptr);
    fail_unless(error == "unsupported inference device: auto");
}
GST_END_TEST

GST_START_TEST(test_runtime_factory_uses_default_cuda_env_device_id) {
    unsetenv("SSV_INFER_CUDA_DEVICE_ID");

    int device_id = -1;
    std::string error;
    fail_unless(ssv::provider::read_cuda_device_id_from_env(&device_id, &error));
    fail_unless(device_id == 0);
    fail_unless(error.empty());

    std::unique_ptr<ssv::provider::Runtime> runtime =
        ssv::provider::create_runtime("onnx", "gpu", &error);
    fail_unless(runtime != nullptr);
    fail_unless(error.empty());
}
GST_END_TEST

GST_START_TEST(test_runtime_factory_rejects_invalid_cuda_env_device_id) {
    setenv("SSV_INFER_CUDA_DEVICE_ID", "abc", 1);

    std::string error;
    int device_id = -1;
    fail_if(ssv::provider::read_cuda_device_id_from_env(&device_id, &error));
    fail_unless(error.find("SSV_INFER_CUDA_DEVICE_ID") != std::string::npos);
    unsetenv("SSV_INFER_CUDA_DEVICE_ID");
}
GST_END_TEST

GST_START_TEST(test_ssvinfer_exposes_runtime_properties) {
    GstElement *element = gst_element_factory_make("ssvinfer", nullptr);
    fail_unless(element != nullptr);

    gchar *runtime = nullptr;
    gchar *device = nullptr;
    g_object_get(element,
        "runtime", &runtime,
        "device", &device,
        nullptr);
    fail_unless(runtime != nullptr);
    fail_unless(device != nullptr);
    fail_unless(std::string(runtime) == "onnx");
    fail_unless(std::string(device) == "cpu");
    fail_unless(g_object_class_find_property(G_OBJECT_GET_CLASS(element), "cuda-device-id") == nullptr);
    g_free(runtime);
    g_free(device);

    g_object_set(element,
        "runtime", "onnx",
        "device", "gpu",
        nullptr);
    g_object_get(element,
        "runtime", &runtime,
        "device", &device,
        nullptr);
    fail_unless(runtime != nullptr);
    fail_unless(device != nullptr);
    fail_unless(std::string(runtime) == "onnx");
    fail_unless(std::string(device) == "gpu");

    g_free(runtime);
    g_free(device);
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
    tcase_add_test(tc, test_ssv_config_reads_runtime_and_device_options);
    tcase_add_test(tc, test_runtime_factory_accepts_onnx_cpu);
    tcase_add_test(tc, test_runtime_factory_rejects_unknown_device);
    tcase_add_test(tc, test_runtime_factory_uses_default_cuda_env_device_id);
    tcase_add_test(tc, test_runtime_factory_rejects_invalid_cuda_env_device_id);
    tcase_add_test(tc, test_ssvinfer_exposes_runtime_properties);
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
