#include "ssv_inference_service.hpp"
#include "ssv_inference_test_service.hpp"
#include "ssv_meta.hpp"

#include <gst/check/gstcheck.h>
#include <gst/gst.h>
#include <gst/app/app.h>
#include <gst/video/video.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

extern void run_overlay_renderer_contract_tests();

static void assert_element_factory(const char *name) {
    GstElement *element = gst_element_factory_make(name, nullptr);
    fail_unless(element != nullptr, "missing element factory: %s", name);
    gst_object_unref(element);
}

static SsvDetectionFrame make_plugin_detection_frame(
    const char *source_id,
    std::uint64_t frame_id,
    GstClockTime pts,
    std::uint64_t generation)
{
    SsvDetectionFrame frame;
    frame.frame_id = frame_id;
    frame.source_id = source_id;
    frame.timing = {pts, GST_SECOND / 30, generation};
    SsvDetection object;
    std::snprintf(object.class_name, sizeof(object.class_name), "person");
    object.confidence = 0.9F;
    object.x1 = 0.1F;
    object.y1 = 0.1F;
    object.x2 = 0.4F;
    object.y2 = 0.4F;
    object.class_id = 0;
    frame.detections.push_back(object);
    return frame;
}

static GstBuffer *make_plugin_rgba_buffer(GstClockTime pts)
{
    GstBuffer *buffer = gst_buffer_new_allocate(
        nullptr, 64 * 48 * 4, nullptr);
    fail_unless(buffer != nullptr);
    GST_BUFFER_PTS(buffer) = pts;
    GST_BUFFER_DURATION(buffer) = GST_SECOND / 30;
    return buffer;
}

GST_START_TEST(test_ssv_plugin_factories_are_registered) {
    assert_element_factory("ssvtemplate");
    assert_element_factory("ssvinfer");
    assert_element_factory("ssvtrack");
    assert_element_factory("ssvpub");
    assert_element_factory("ssvoverlay");
}
GST_END_TEST

GST_START_TEST(test_ssvtrack_links_to_ssvpub_for_rgba_analysis) {
    GstElement *tracker = gst_element_factory_make("ssvtrack", nullptr);
    GstElement *publisher = gst_element_factory_make("ssvpub", nullptr);
    fail_unless(tracker != nullptr);
    fail_unless(publisher != nullptr);

    fail_unless(gst_element_link(tracker, publisher));

    gst_object_unref(publisher);
    gst_object_unref(tracker);
}
GST_END_TEST

GST_START_TEST(test_source_geometry_change_resets_infer_and_tracker_generation) {
    const char *source_id = "plugin-geometry-reset-test";
    const char *label_map = std::getenv("SSV_TEST_LABEL_MAP_PATH");
    fail_unless(label_map != nullptr && label_map[0] != '\0');
    ssv::SsvInferenceConfig config;
    config.model.path = "/bin/true";
    config.model.output_format = "yolo_nx6";
    config.model.label_map = label_map;
    config.target_class = "person";
    ssv::infer::SsvInferenceTestServiceOptions service_options;
    service_options.detection_sequence = {
        {.x1 = 0.05F, .x2 = 0.15F},
        {.x1 = 0.80F, .x2 = 0.90F},
        {.x1 = 0.40F, .x2 = 0.50F},
    };
    auto service = ssv::infer::ssv_inference_test_service_create(
        config, std::move(service_options));
    ssv::infer::ssv_inference_service_update_source_geometry(
        service.get(), source_id, 6, 4);

    GError *error = nullptr;
    GstElement *pipeline = gst_parse_launch(
        "appsrc name=src format=time is-live=false block=true ! "
        "video/x-raw,format=RGBA,width=3,height=2,framerate=15/1 ! "
        "ssvinfer name=infer source-id=plugin-geometry-reset-test ! "
        "ssvtrack source-id=plugin-geometry-reset-test gmc-method=none ! "
        "appsink name=sink sync=false emit-signals=false",
        &error);
    fail_unless(error == nullptr);
    fail_unless(pipeline != nullptr);
    GstElement *src = gst_bin_get_by_name(GST_BIN(pipeline), "src");
    GstElement *infer = gst_bin_get_by_name(GST_BIN(pipeline), "infer");
    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    fail_unless(src != nullptr && infer != nullptr && sink != nullptr);
    g_object_set(infer, "inference-service", service.get(), nullptr);
    fail_unless(gst_element_set_state(pipeline, GST_STATE_PLAYING)
        != GST_STATE_CHANGE_FAILURE);

    auto push_and_track = [&](GstClockTime pts, int expected_track_id) {
        GstBuffer *buffer = gst_buffer_new_allocate(nullptr, 24, nullptr);
        fail_unless(buffer != nullptr);
        GST_BUFFER_PTS(buffer) = pts;
        GST_BUFFER_DURATION(buffer) = GST_SECOND / 15;
        fail_unless(gst_app_src_push_buffer(GST_APP_SRC(src), buffer)
            == GST_FLOW_OK);
        GstSample *sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
        fail_unless(sample != nullptr);
        gst_sample_unref(sample);

        auto tracked = ssv_meta(source_id)->consume_tracked();
        fail_unless(tracked.result == SsvMetaResult::Consumed);
        fail_unless(tracked.frame != nullptr);
        fail_unless(tracked.frame->objects.size() == 1);
        fail_unless(
            tracked.frame->objects.front().track_id == expected_track_id);
        return tracked.frame->timing.generation;
    };

    const auto first_generation = push_and_track(GST_SECOND, 1);
    fail_unless(push_and_track(2 * GST_SECOND, 2) == first_generation);
    ssv::infer::ssv_inference_service_update_source_geometry(
        service.get(), source_id, 3, 2);
    fail_unless(
        push_and_track(3 * GST_SECOND, 1) == first_generation + 1);

    gst_app_src_end_of_stream(GST_APP_SRC(src));
    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *message = gst_bus_timed_pop_filtered(
        bus, 5 * GST_SECOND,
        static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
    fail_unless(message != nullptr);
    fail_unless(GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS);

    gst_message_unref(message);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(src);
    gst_object_unref(infer);
    gst_object_unref(sink);
    gst_object_unref(pipeline);
    ssv::infer::ssv_inference_service_stop(service.get());
}
GST_END_TEST

#if SSV_HAS_OPENCV
GST_START_TEST(test_infer_and_sparse_gmc_share_one_analysis_frame_map) {
    const char *source_id = "plugin-infer-track-lease-test";
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
        "ssvinfer name=infer source-id=plugin-infer-track-lease-test ! "
        "ssvtrack source-id=plugin-infer-track-lease-test "
        "gmc-method=sparse-opt-flow gmc-downscale=1 ! "
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

    auto tracked = ssv_meta(source_id)->consume_tracked();
    fail_unless(tracked.result == SsvMetaResult::Consumed);
    fail_unless(tracked.frame != nullptr);
    fail_unless(tracked.frame->objects.size() == 1);
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
#endif

GST_START_TEST(test_infer_track_preserve_controlled_buffer_timing) {
    const char *source_id = "plugin-timing-test";
    GstElement *pipeline = gst_parse_launch(
        "videotestsrc num-buffers=1 ! "
        "video/x-raw,format=RGBA,width=64,height=48,framerate=30/1 ! "
        "ssvinfer source-id=plugin-timing-test mock-detect=true ! "
        "ssvtrack source-id=plugin-timing-test mock-track=true gmc-method=none ! "
        "fakesink sync=false",
        nullptr);
    fail_unless(pipeline != nullptr);
    fail_unless(gst_element_set_state(pipeline, GST_STATE_PLAYING) !=
        GST_STATE_CHANGE_FAILURE);

    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *message = gst_bus_timed_pop_filtered(
        bus, 5 * GST_SECOND,
        static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
    fail_unless(message != nullptr);
    fail_unless(GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS);

    auto meta = ssv_meta(source_id);
    const auto generation = meta->generation();
    auto snapshot = meta->latest_tracked_at_or_before(GST_SECOND);
    fail_unless(snapshot != nullptr);
    fail_unless(snapshot->frame_id == 0);
    fail_unless(snapshot->source_id == source_id);
    fail_unless(snapshot->timing.pts == 0);
    fail_unless(snapshot->timing.duration == GST_SECOND / 30);
    fail_unless(snapshot->timing.generation == generation);
    fail_unless(snapshot->objects.size() == 1);
    fail_unless(snapshot->objects.front().track_id == 1);

    gst_message_unref(message);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}
GST_END_TEST

GST_START_TEST(test_ssvtrack_rejects_wrong_source_observation) {
    const char *bound_source = "plugin-wrong-source-a";
    auto meta = ssv_meta(bound_source);
    SsvTimelineCursor timeline(meta);
    const auto update = timeline.on_segment({0, 0, 0, 1.0});

    GstElement *pipeline = gst_parse_launch(
        "appsrc name=src format=time is-live=false block=true ! "
        "video/x-raw,format=RGBA,width=64,height=48,framerate=30/1 ! "
        "ssvtrack name=track source-id=plugin-wrong-source-a "
        "mock-track=true gmc-method=none ! fakesink sync=false",
        nullptr);
    fail_unless(pipeline != nullptr);
    GstElement *src = gst_bin_get_by_name(GST_BIN(pipeline), "src");
    GstElement *track = gst_bin_get_by_name(GST_BIN(pipeline), "track");
    fail_unless(src != nullptr);
    fail_unless(track != nullptr);
    fail_unless(gst_element_set_state(pipeline, GST_STATE_PLAYING) !=
        GST_STATE_CHANGE_FAILURE);

    g_object_set(track, "source-id", "plugin-wrong-source-b", nullptr);
    gchar *configured_source = nullptr;
    g_object_get(track, "source-id", &configured_source, nullptr);
    fail_unless(configured_source != nullptr);
    fail_unless(std::string(configured_source) == "plugin-wrong-source-b");
    g_free(configured_source);

    auto detection = make_plugin_detection_frame(
        bound_source, 0, GST_SECOND, update.generation);
    fail_unless(meta->publish_detection(std::move(detection)) ==
        SsvMetaResult::Published);

    fail_unless(gst_app_src_push_buffer(
        GST_APP_SRC(src), make_plugin_rgba_buffer(GST_SECOND)) == GST_FLOW_OK);
    gst_app_src_end_of_stream(GST_APP_SRC(src));

    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *message = gst_bus_timed_pop_filtered(
        bus, 5 * GST_SECOND,
        static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
    fail_unless(message != nullptr);
    fail_unless(GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR);

    gst_message_unref(message);
    gst_object_unref(bus);
    gst_object_unref(src);
    gst_object_unref(track);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}
GST_END_TEST

GST_START_TEST(test_ssvtrack_rejects_real_tracking_without_analysis_frame) {
    const char *source_id = "plugin-missing-analysis-frame-test";
    auto meta = ssv_meta(source_id);
    SsvTimelineCursor timeline(meta);
    const auto update = timeline.on_segment({0, 0, 0, 1.0});

    GstElement *pipeline = gst_parse_launch(
        "appsrc name=src format=time is-live=false block=true ! "
        "video/x-raw,format=RGBA,width=64,height=48,framerate=30/1 ! "
        "ssvtrack source-id=plugin-missing-analysis-frame-test "
        "gmc-method=none ! fakesink sync=false",
        nullptr);
    fail_unless(pipeline != nullptr);
    GstElement *src = gst_bin_get_by_name(GST_BIN(pipeline), "src");
    fail_unless(src != nullptr);
    fail_unless(gst_element_set_state(pipeline, GST_STATE_PLAYING)
        != GST_STATE_CHANGE_FAILURE);

    auto detection = make_plugin_detection_frame(
        source_id, 0, GST_SECOND, update.generation);
    fail_unless(detection.analysis_frame == nullptr);
    fail_unless(meta->publish_detection(std::move(detection))
        == SsvMetaResult::Published);
    fail_unless(gst_app_src_push_buffer(
        GST_APP_SRC(src), make_plugin_rgba_buffer(GST_SECOND)) == GST_FLOW_OK);
    gst_app_src_end_of_stream(GST_APP_SRC(src));

    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *message = gst_bus_timed_pop_filtered(
        bus, 5 * GST_SECOND,
        static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
    fail_unless(message != nullptr);
    fail_unless(GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR);

    gst_message_unref(message);
    gst_object_unref(bus);
    gst_object_unref(src);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}
GST_END_TEST

GST_START_TEST(test_ssvtrack_resets_real_state_for_consumed_generation) {
    const char *source_id = "plugin-track-generation-test";
    auto meta = ssv_meta(source_id);
    SsvTimelineCursor timeline(meta);
    auto update = timeline.on_segment({0, 0, 0, 1.0});
    GstVideoInfo video_info;
    fail_unless(gst_video_info_set_format(
        &video_info, GST_VIDEO_FORMAT_RGBA, 64, 48));
    SsvAnalysisFramePool analysis_pool(64, 48, 2);

    GstElement *pipeline = gst_parse_launch(
        "appsrc name=src format=time is-live=false block=true ! "
        "video/x-raw,format=RGBA,width=64,height=48,framerate=30/1 ! "
        "ssvtrack source-id=plugin-track-generation-test "
        "gmc-method=none ! "
        "appsink name=sink sync=false emit-signals=false",
        nullptr);
    fail_unless(pipeline != nullptr);
    GstElement *src = gst_bin_get_by_name(GST_BIN(pipeline), "src");
    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    fail_unless(src != nullptr);
    fail_unless(sink != nullptr);
    fail_unless(gst_element_set_state(pipeline, GST_STATE_PLAYING) !=
        GST_STATE_CHANGE_FAILURE);

    auto publish_and_process = [&](std::uint64_t frame_id,
                                   GstClockTime pts,
                                   std::uint64_t generation,
                                   float x1,
                                   float x2,
                                   int expected_track_id) {
        GstBuffer *buffer = make_plugin_rgba_buffer(pts);
        auto detection = make_plugin_detection_frame(
            source_id, frame_id, pts, generation);
        detection.detections.front().x1 = x1;
        detection.detections.front().x2 = x2;
        detection.analysis_frame = analysis_pool.create(
            buffer,
            video_info,
            {64, 48, 64, 48, 1.0F, 0, 0, 0, 0},
            detection.timing);
        fail_unless(meta->publish_detection(std::move(detection)) ==
            SsvMetaResult::Published);

        fail_unless(gst_app_src_push_buffer(
            GST_APP_SRC(src), buffer) == GST_FLOW_OK);
        GstSample *sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
        fail_unless(sample != nullptr);
        gst_sample_unref(sample);

        auto tracked = meta->consume_tracked();
        fail_unless(tracked.result == SsvMetaResult::Consumed);
        fail_unless(tracked.frame != nullptr);
        fail_unless(tracked.frame->timing.generation == generation);
        fail_unless(tracked.frame->objects.size() == 1);
        fail_unless(
            tracked.frame->objects.front().track_id == expected_track_id);
        fail_unless(analysis_pool.stats().active_maps == 0);
        fail_unless(analysis_pool.stats().outstanding_staging_leases == 0);
    };

    publish_and_process(
        1, GST_SECOND, update.generation, 0.05F, 0.15F, 1);
    publish_and_process(
        2, 2 * GST_SECOND, update.generation, 0.8F, 0.9F, 2);
    update = timeline.on_lifecycle_reset();
    publish_and_process(
        3, 3 * GST_SECOND, update.generation, 0.4F, 0.5F, 1);
    fail_unless(analysis_pool.stats().map_count == 3);

    gst_app_src_end_of_stream(GST_APP_SRC(src));
    gst_object_unref(src);
    gst_object_unref(sink);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}
GST_END_TEST

GST_START_TEST(test_perception_plugins_reject_mismatched_source_context) {
    const char *plugin_names[] = {
        "ssvinfer", "ssvtrack", "ssvpub", "ssvoverlay"};
    auto mismatched_context = std::make_shared<SsvSourceContext>(
        "different-source");
    for (const char *plugin_name : plugin_names) {
        GstElement *element = gst_element_factory_make(plugin_name, nullptr);
        fail_unless(element != nullptr);
        g_object_set(
            element,
            "source-id", "camera-01",
            "source-context", mismatched_context.get(),
            nullptr);
        if (std::string(plugin_name) == "ssvinfer")
            g_object_set(element, "mock-detect", TRUE, nullptr);
        else if (std::string(plugin_name) == "ssvtrack")
            g_object_set(element, "gmc-method", "none", nullptr);

        GstElement *pipeline = gst_pipeline_new(nullptr);
        fail_unless(pipeline != nullptr);
        fail_unless(gst_bin_add(GST_BIN(pipeline), element));
        GstBus *bus = gst_element_get_bus(pipeline);
        fail_unless(bus != nullptr);
        gst_element_set_state(pipeline, GST_STATE_PAUSED);
        GstMessage *message = gst_bus_timed_pop_filtered(
            bus,
            GST_SECOND,
            static_cast<GstMessageType>(GST_MESSAGE_ERROR));
        fail_unless(message != nullptr,
            "plugin %s did not report a mismatch error", plugin_name);
        GError *error = nullptr;
        gchar *debug = nullptr;
        gst_message_parse_error(message, &error, &debug);
        fail_unless(error != nullptr);
        fail_unless(
            std::string(error->message).find(
                "source-context does not match source-id")
                != std::string::npos,
            "plugin %s reported the wrong error: %s",
            plugin_name, error->message);
        g_clear_error(&error);
        g_free(debug);
        gst_message_unref(message);
        gst_object_unref(bus);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
    }
}
GST_END_TEST

GST_START_TEST(test_perception_plugins_share_source_id_contract) {
    const char *plugin_names[] = {"ssvinfer", "ssvtrack", "ssvpub", "ssvoverlay"};
    auto source_context = std::make_shared<SsvSourceContext>("camera-01");
    for (const char *plugin_name : plugin_names) {
        GstElement *element = gst_element_factory_make(plugin_name, nullptr);
        fail_unless(element != nullptr);
        gchar *source_id = nullptr;
        g_object_get(element, "source-id", &source_id, nullptr);
        fail_unless(source_id != nullptr);
        fail_unless(std::string(source_id) == "pipeline-0");
        g_free(source_id);

        g_object_set(element, "source-id", "camera-01", nullptr);
        g_object_get(element, "source-id", &source_id, nullptr);
        fail_unless(source_id != nullptr);
        fail_unless(std::string(source_id) == "camera-01");
        g_free(source_id);

        fail_unless(g_object_class_find_property(
            G_OBJECT_GET_CLASS(element), "source-context") != nullptr);
        g_object_set(element, "source-context", source_context.get(), nullptr);
        gpointer configured_context = nullptr;
        g_object_get(element, "source-context", &configured_context, nullptr);
        fail_unless(configured_context == source_context.get());
        gst_object_unref(element);
    }

    GstElement *overlay = gst_element_factory_make("ssvoverlay", nullptr);
    gboolean motion_prediction = FALSE;
    guint max_horizon_ms = 0;
    gchar *font_face = nullptr;
    guint font_size = 0;
    g_object_get(overlay,
        "motion-prediction", &motion_prediction,
        "max-horizon-ms", &max_horizon_ms,
        "font-face", &font_face,
        "font-size", &font_size,
        nullptr);
    fail_unless(motion_prediction);
    fail_unless(max_horizon_ms == 300);
    fail_unless(font_face != nullptr);
    fail_unless(std::string(font_face) == "regular");
    fail_unless(font_size == 7);
    g_free(font_face);
    g_object_set(overlay,
        "motion-prediction", FALSE,
        "max-horizon-ms", 125U,
        "font-face", "bold",
        "font-size", 14U,
        nullptr);
    g_object_get(overlay,
        "motion-prediction", &motion_prediction,
        "max-horizon-ms", &max_horizon_ms,
        "font-face", &font_face,
        "font-size", &font_size,
        nullptr);
    fail_unless(!motion_prediction);
    fail_unless(max_horizon_ms == 125);
    fail_unless(font_face != nullptr);
    fail_unless(std::string(font_face) == "bold");
    fail_unless(font_size == 14);
    g_free(font_face);
    gst_object_unref(overlay);
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

GST_START_TEST(test_ssvtrack_rejects_invalid_gmc_method_on_start) {
    GstElement *pipeline = gst_parse_launch(
        "videotestsrc num-buffers=1 ! "
        "video/x-raw,format=RGBA,width=64,height=48 ! "
        "ssvtrack mock-track=true gmc-method=invalid ! fakesink sync=false",
        nullptr);
    fail_unless(pipeline != nullptr);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *message = gst_bus_timed_pop_filtered(
        bus, GST_SECOND,
        static_cast<GstMessageType>(GST_MESSAGE_ERROR));
    fail_unless(message != nullptr);

    gst_message_unref(message);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}
GST_END_TEST

#if !SSV_HAS_OPENCV
GST_START_TEST(test_ssvtrack_rejects_sparse_gmc_without_opencv_on_start) {
    GstElement *pipeline = gst_parse_launch(
        "videotestsrc num-buffers=1 ! "
        "video/x-raw,format=RGBA,width=64,height=48 ! "
        "ssvtrack gmc-method=sparse-opt-flow ! fakesink sync=false",
        nullptr);
    fail_unless(pipeline != nullptr);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *message = gst_bus_timed_pop_filtered(
        bus, GST_SECOND,
        static_cast<GstMessageType>(GST_MESSAGE_ERROR));
    fail_unless(message != nullptr);

    gst_message_unref(message);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}
GST_END_TEST
#endif

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
    const char *source_id = "overlay-draw-test";
    GstElement *pipeline = gst_parse_launch(
        "appsrc name=src format=time is-live=false block=true ! "
        "video/x-raw,format=BGRx,width=64,height=48,framerate=30/1 ! "
        "ssvoverlay source-id=overlay-draw-test motion-prediction=false ! "
        "appsink name=sink sync=false emit-signals=false max-buffers=1 drop=false",
        nullptr);
    fail_unless(pipeline != nullptr);

    GstElement *src = gst_bin_get_by_name(GST_BIN(pipeline), "src");
    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    fail_unless(src != nullptr);
    fail_unless(sink != nullptr);

    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    fail_unless(ret != GST_STATE_CHANGE_FAILURE);

    auto make_black_buffer = [](GstClockTime pts) {
        constexpr gsize frame_size = 64 * 48 * 4;
        GstBuffer *buffer = gst_buffer_new_allocate(nullptr, frame_size, nullptr);
        fail_unless(buffer != nullptr);
        gst_buffer_memset(buffer, 0, 0, frame_size);
        GST_BUFFER_PTS(buffer) = pts;
        GST_BUFFER_DURATION(buffer) = GST_SECOND / 30;
        return buffer;
    };

    fail_unless(gst_app_src_push_buffer(
        GST_APP_SRC(src), make_black_buffer(0)) == GST_FLOW_OK);
    GstSample *sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
    fail_unless(sample != nullptr);
    gst_sample_unref(sample);

    auto meta = ssv_meta(source_id);
    const auto generation = meta->generation();
    fail_unless(generation > 0);

    SsvDetectionFrame observation;
    observation.frame_id = 1;
    observation.source_id = source_id;
    observation.timing = {GST_SECOND / 30, GST_SECOND / 30, generation};
    SsvTrackedObject object;
    std::snprintf(
        object.detection.class_name,
        sizeof(object.detection.class_name),
        "person");
    object.detection.confidence = 0.9F;
    object.detection.x1 = 0.1F;
    object.detection.y1 = 0.1F;
    object.detection.x2 = 0.4F;
    object.detection.y2 = 0.4F;
    object.detection.class_id = 0;
    object.track_id = 7;
    object.track_state = SSV_TRACK_MATCHED;
    fail_unless(meta->publish_tracked(
                    std::move(observation), {std::move(object)}) ==
        SsvMetaResult::Published);

    fail_unless(gst_app_src_push_buffer(
        GST_APP_SRC(src), make_black_buffer(GST_SECOND / 30)) == GST_FLOW_OK);
    sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
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
    gst_object_unref(src);
    gst_object_unref(sink);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}
GST_END_TEST

GST_START_TEST(test_ssvoverlay_tracks_controlled_timeline_events) {
    const char *source_id = "overlay-timeline-test";
    GstElement *pipeline = gst_parse_launch(
        "appsrc name=src format=time is-live=false block=true ! "
        "video/x-raw,format=BGRx,width=64,height=48,framerate=30/1 ! "
        "ssvoverlay source-id=overlay-timeline-test ! "
        "appsink name=sink sync=false emit-signals=false max-buffers=1 drop=false",
        nullptr);
    fail_unless(pipeline != nullptr);

    GstElement *src = gst_bin_get_by_name(GST_BIN(pipeline), "src");
    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    fail_unless(src != nullptr);
    fail_unless(sink != nullptr);
    fail_unless(gst_element_set_state(pipeline, GST_STATE_PLAYING) !=
        GST_STATE_CHANGE_FAILURE);

    auto push_and_pull = [&](GstClockTime pts, bool discontinuity) {
        constexpr gsize frame_size = 64 * 48 * 4;
        GstBuffer *buffer = gst_buffer_new_allocate(nullptr, frame_size, nullptr);
        fail_unless(buffer != nullptr);
        gst_buffer_memset(buffer, 0, 0, frame_size);
        GST_BUFFER_PTS(buffer) = pts;
        GST_BUFFER_DURATION(buffer) = GST_SECOND / 30;
        if (discontinuity)
            GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DISCONT);
        fail_unless(gst_app_src_push_buffer(
            GST_APP_SRC(src), buffer) == GST_FLOW_OK);
        GstSample *sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
        fail_unless(sample != nullptr);
        gst_sample_unref(sample);
    };

    push_and_pull(100 * GST_MSECOND, false);
    auto meta = ssv_meta(source_id);
    const auto initial_generation = meta->generation();
    fail_unless(initial_generation > 0);

    push_and_pull(GST_CLOCK_TIME_NONE, false);
    fail_unless(meta->generation() == initial_generation);

    push_and_pull(200 * GST_MSECOND, true);
    const auto discontinuity_generation = meta->generation();
    fail_unless(discontinuity_generation == initial_generation + 1);

    push_and_pull(300 * GST_MSECOND, false);
    push_and_pull(250 * GST_MSECOND, false);
    const auto rollback_generation = meta->generation();
    fail_unless(rollback_generation == discontinuity_generation + 1);

    GstPad *src_pad = gst_element_get_static_pad(src, "src");
    fail_unless(src_pad != nullptr);
    fail_unless(gst_pad_push_event(src_pad, gst_event_new_flush_start()));
    fail_unless(gst_pad_push_event(src_pad, gst_event_new_flush_stop(TRUE)));
    gst_object_unref(src_pad);
    const auto flush_generation = meta->generation();
    fail_unless(flush_generation == rollback_generation + 1);

    fail_unless(gst_element_set_state(pipeline, GST_STATE_NULL) !=
        GST_STATE_CHANGE_FAILURE);
    const auto stopped_generation = meta->generation();
    fail_unless(stopped_generation == flush_generation + 1);

    fail_unless(gst_element_set_state(pipeline, GST_STATE_PLAYING) !=
        GST_STATE_CHANGE_FAILURE);
    push_and_pull(400 * GST_MSECOND, false);
    fail_unless(meta->generation() >= stopped_generation);

    gst_object_unref(src);
    gst_object_unref(sink);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}
GST_END_TEST

static Suite *ssv_gst_suite() {
    Suite *suite = suite_create("ssv-gst");
    TCase *tc = tcase_create("plugins");
    tcase_add_test(tc, test_ssv_plugin_factories_are_registered);
    tcase_add_test(tc, test_ssvtrack_links_to_ssvpub_for_rgba_analysis);
    tcase_add_test(tc, test_infer_track_preserve_controlled_buffer_timing);
    tcase_add_test(tc, test_ssvtrack_rejects_wrong_source_observation);
    tcase_add_test(
        tc, test_ssvtrack_rejects_real_tracking_without_analysis_frame);
    tcase_add_test(tc, test_ssvtrack_resets_real_state_for_consumed_generation);
    tcase_add_test(
        tc, test_perception_plugins_reject_mismatched_source_context);
    tcase_add_test(tc, test_perception_plugins_share_source_id_contract);
    tcase_add_test(
        tc, test_source_geometry_change_resets_infer_and_tracker_generation);
#if SSV_HAS_OPENCV
    tcase_add_test(
        tc, test_infer_and_sparse_gmc_share_one_analysis_frame_map);
#endif
    tcase_add_test(tc, test_ssvtrack_exposes_botsort_properties);
    tcase_add_test(tc, test_ssvtrack_rejects_invalid_gmc_method_on_start);
#if !SSV_HAS_OPENCV
    tcase_add_test(
        tc, test_ssvtrack_rejects_sparse_gmc_without_opencv_on_start);
#endif
    tcase_add_test(tc, test_ssvoverlay_runs_on_rgb_buffer);
    tcase_add_test(tc, test_ssvoverlay_runs_on_bgrx_buffer);
    tcase_add_test(tc, test_ssvoverlay_draws_latest_detection);
    tcase_add_test(tc, test_ssvoverlay_tracks_controlled_timeline_events);
    suite_add_tcase(suite, tc);
    return suite;
}

int main(int argc, char **argv) {
    gst_check_init(&argc, &argv);
    run_overlay_renderer_contract_tests();

    Suite *suite = ssv_gst_suite();
    SRunner *runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed == 0 ? 0 : 1;
}
