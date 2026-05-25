#include "ssv_meta.hpp"

#include <cassert>
#include <cstdio>

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

void run_ssv_meta_tests() {
    test_detection_store_tracking_flow();
    test_detection_store_overwrites_stale_detection();
}
