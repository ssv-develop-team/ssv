#include "ssv_meta.hpp"

#include <cmath>
#include <cassert>
#include <cstdio>
#include <limits>

static SsvFrameDetections make_detection(guint64 frame_id, int track_id = -1) {
    SsvFrameDetections frame;
    frame.frame_id = frame_id;
    std::snprintf(frame.source_id, sizeof(frame.source_id), "unit-test");

    SsvDetection det{};
    std::snprintf(det.class_name, sizeof(det.class_name), "person");
    det.confidence = 0.9f;
    det.x1 = 0.1f;
    det.y1 = 0.2f;
    det.x2 = 0.3f;
    det.y2 = 0.4f;
    det.class_id = 0;
    det.track_id = track_id;
    frame.detections.push_back(det);
    return frame;
}

static void assert_single_detection(const SsvFrameDetections &frame,
                                    float x1, float y1, float x2, float y2,
                                    int class_id, int track_id) {
    assert(frame.detections.size() == 1);
    const auto &det = frame.detections[0];
    assert(std::fabs(det.x1 - x1) < 0.0001f);
    assert(std::fabs(det.y1 - y1) < 0.0001f);
    assert(std::fabs(det.x2 - x2) < 0.0001f);
    assert(std::fabs(det.y2 - y2) < 0.0001f);
    assert(det.class_id == class_id);
    assert(det.track_id == track_id);
}

static void test_detection_normalizes_valid_detection() {
    auto frame = make_detection(30);
    auto det = frame.detections[0];

    bool kept = ssv_normalize_detection(det);

    assert(kept);
    assert(std::fabs(det.confidence - 0.9f) < 0.0001f);
    assert(std::fabs(det.x1 - 0.1f) < 0.0001f);
    assert(std::fabs(det.y1 - 0.2f) < 0.0001f);
    assert(std::fabs(det.x2 - 0.3f) < 0.0001f);
    assert(std::fabs(det.y2 - 0.4f) < 0.0001f);
    assert(det.class_id == 0);
    assert(det.track_id == -1);
}

static void test_detection_clamps_slightly_out_of_bounds_bbox() {
    auto frame = make_detection(31);
    auto det = frame.detections[0];
    det.x1 = -0.01f;
    det.y1 = -0.02f;
    det.x2 = 1.01f;
    det.y2 = 1.02f;

    bool kept = ssv_normalize_detection(det);

    assert(kept);
    assert(std::fabs(det.x1 - 0.0f) < 0.0001f);
    assert(std::fabs(det.y1 - 0.0f) < 0.0001f);
    assert(std::fabs(det.x2 - 1.0f) < 0.0001f);
    assert(std::fabs(det.y2 - 1.0f) < 0.0001f);
}

static void test_detection_rejects_invalid_numbers() {
    auto nan_det = make_detection(32).detections[0];
    nan_det.x1 = std::numeric_limits<float>::quiet_NaN();
    assert(!ssv_normalize_detection(nan_det));

    auto inf_det = make_detection(33).detections[0];
    inf_det.confidence = std::numeric_limits<float>::infinity();
    assert(!ssv_normalize_detection(inf_det));

    auto high_conf = make_detection(34).detections[0];
    high_conf.confidence = 1.01f;
    assert(!ssv_normalize_detection(high_conf));

    auto low_conf = make_detection(35).detections[0];
    low_conf.confidence = -0.01f;
    assert(!ssv_normalize_detection(low_conf));
}

static void test_detection_rejects_empty_bbox_after_clamp() {
    auto det = make_detection(36).detections[0];
    det.x1 = -0.2f;
    det.y1 = 0.2f;
    det.x2 = -0.1f;
    det.y2 = 0.4f;

    assert(!ssv_normalize_detection(det));
}

static void test_detection_normalizes_invalid_negative_ids() {
    auto det = make_detection(37).detections[0];
    det.class_id = -9;
    det.track_id = -3;

    bool kept = ssv_normalize_detection(det);

    assert(kept);
    assert(det.class_id == -1);
    assert(det.track_id == -1);
}

static void test_detection_store_tracking_flow() {
    auto &store = SsvDetectionStore::instance();
    (void)store.take();
    (void)store.take_for_tracking();

    store.set(make_detection(10));
    auto for_tracking = store.take_for_tracking();
    assert(for_tracking.frame_id == 10);
    assert(for_tracking.detections.size() == 1);
    assert(for_tracking.detections[0].track_id == -1);

    for_tracking.detections[0].track_id = 7;
    store.set_tracked(std::move(for_tracking));

    auto latest = store.peek_latest();
    assert(latest.frame_id == 10);
    assert(latest.detections.size() == 1);
    assert(latest.detections[0].track_id == 7);

    auto published = store.take();
    assert(published.frame_id == 10);
    assert(published.detections.size() == 1);
    assert(published.detections[0].track_id == 7);

    latest = store.peek_latest();
    assert(latest.frame_id == 10);
    assert(latest.detections.size() == 1);
}

static void test_detection_store_overwrites_stale_detection() {
    auto &store = SsvDetectionStore::instance();
    (void)store.take();
    (void)store.take_for_tracking();

    store.set(make_detection(20));
    store.set(make_detection(21));

    auto for_tracking = store.take_for_tracking();
    assert(for_tracking.frame_id == 21);
}

static void test_detection_store_filters_invalid_detection() {
    auto &store = SsvDetectionStore::instance();
    (void)store.take();
    (void)store.take_for_tracking();

    auto frame = make_detection(40);
    auto invalid = frame.detections[0];
    invalid.x1 = 0.8f;
    invalid.x2 = 0.7f;
    frame.detections.push_back(invalid);

    store.set(std::move(frame));

    auto for_tracking = store.take_for_tracking();
    assert(for_tracking.frame_id == 40);
    assert_single_detection(for_tracking, 0.1f, 0.2f, 0.3f, 0.4f, 0, -1);
}

void run_ssv_meta_tests() {
    test_detection_normalizes_valid_detection();
    test_detection_clamps_slightly_out_of_bounds_bbox();
    test_detection_rejects_invalid_numbers();
    test_detection_rejects_empty_bbox_after_clamp();
    test_detection_normalizes_invalid_negative_ids();
    test_detection_store_tracking_flow();
    test_detection_store_overwrites_stale_detection();
    test_detection_store_filters_invalid_detection();
}
