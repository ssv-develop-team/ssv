#include "ssv_meta.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>

#include <nlohmann/json.hpp>

std::string ssv_pub_build_event_payload(const SsvFrameDetections &det, std::int64_t timestamp_ms);
bool ssv_overlay_glyph_is_supported(char c);

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

/// Assert that det is valid and has the given track_id.
static void assert_detection_track_id(const SsvDetection &det, int expected_track_id) {
    assert(std::isfinite(det.confidence));
    assert(det.confidence >= 0.0f && det.confidence <= 1.0f);
    assert(det.track_id == expected_track_id);
}

/// Assert tracking metadata fields (track_id, track_state, occluded).
static void assert_detection_tracking_state(const SsvDetection &det,
                                            int expected_track_id,
                                            int expected_track_state,
                                            bool expected_occluded) {
    assert_detection_track_id(det, expected_track_id);
    assert(det.track_state == expected_track_state);
    assert(det.occluded == expected_occluded);
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

static void test_detection_normalizes_invalid_track_state() {
    auto det = make_detection(38).detections[0];
    det.track_state = -1;
    assert(ssv_normalize_detection(det));
    assert(det.track_state == SSV_TRACK_NEW);

    det.track_state = 99;
    assert(ssv_normalize_detection(det));
    assert(det.track_state == SSV_TRACK_NEW);
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

// ── M2/B: Tracking metadata contract tests ─────────────────────────────

static void test_track_id_write_back_multiple_detections() {
    auto &store = SsvDetectionStore::instance();
    (void)store.take();
    (void)store.take_for_tracking();

    // Two detections in one frame
    SsvFrameDetections frame;
    frame.frame_id = 100;
    std::snprintf(frame.source_id, sizeof(frame.source_id), "track-test");

    SsvDetection det1{};
    std::snprintf(det1.class_name, sizeof(det1.class_name), "person");
    det1.confidence = 0.9f;
    det1.x1 = 0.1f; det1.y1 = 0.2f; det1.x2 = 0.3f; det1.y2 = 0.4f;
    det1.class_id = 0;

    SsvDetection det2{};
    std::snprintf(det2.class_name, sizeof(det2.class_name), "helmet");
    det2.confidence = 0.8f;
    det2.x1 = 0.5f; det2.y1 = 0.5f; det2.x2 = 0.7f; det2.y2 = 0.8f;
    det2.class_id = 1;

    frame.detections.push_back(det1);
    frame.detections.push_back(det2);
    store.set(std::move(frame));

    // Simulate ssvtrack: take for tracking, assign track_ids, write back
    auto for_tracking = store.take_for_tracking();
    assert(for_tracking.frame_id == 100);
    assert(for_tracking.detections.size() == 2);
    assert_detection_tracking_state(for_tracking.detections[0], -1, SSV_TRACK_NEW, false);
    assert_detection_tracking_state(for_tracking.detections[1], -1, SSV_TRACK_NEW, false);

    // ssvtrack assigns track IDs with state/occlusion
    for_tracking.detections[0].track_id = 5;
    for_tracking.detections[0].track_state = SSV_TRACK_MATCHED;
    for_tracking.detections[0].occluded = false;
    for_tracking.detections[1].track_id = 7;
    for_tracking.detections[1].track_state = SSV_TRACK_NEW;
    for_tracking.detections[1].occluded = true;
    store.set_tracked(std::move(for_tracking));

    // ssvpub: take for publishing
    auto tracked = store.take();
    assert(tracked.frame_id == 100);
    assert(tracked.detections.size() == 2);
    assert_detection_tracking_state(tracked.detections[0], 5, SSV_TRACK_MATCHED, false);
    assert_detection_tracking_state(tracked.detections[1], 7, SSV_TRACK_NEW, true);
}

static void test_track_id_default_preserved() {
    auto &store = SsvDetectionStore::instance();
    (void)store.take();
    (void)store.take_for_tracking();

    store.set(make_detection(110));

    auto for_tracking = store.take_for_tracking();
    assert(for_tracking.frame_id == 110);
    assert(for_tracking.detections.size() == 1);
    // Untracked default: track_id=-1, state=NEW(0), occluded=false
    assert_detection_tracking_state(for_tracking.detections[0], -1, SSV_TRACK_NEW, false);

    // Simulate ssvtrack returning data without touching track_id/state/occluded
    store.set_tracked(std::move(for_tracking));

    auto tracked = store.take();
    assert(tracked.frame_id == 110);
    assert(tracked.detections.size() == 1);
    assert_detection_tracking_state(tracked.detections[0], -1, SSV_TRACK_NEW, false);
}

static void test_store_skips_set_when_has_tracks() {
    auto &store = SsvDetectionStore::instance();
    (void)store.take();
    (void)store.take_for_tracking();

    // Put one frame into HAS_TRACKS with full tracking state
    store.set(make_detection(120));
    auto f = store.take_for_tracking();
    f.detections[0].track_id = 42;
    f.detections[0].track_state = SSV_TRACK_MATCHED;
    f.detections[0].occluded = true;
    store.set_tracked(std::move(f));

    // Now try to set a new detection — should be skipped (HAS_TRACKS protected)
    store.set(make_detection(121));

    // take() should still return frame 120 with unchanged tracking state
    auto result = store.take();
    assert(result.frame_id == 120);
    assert(result.detections.size() == 1);
    assert_detection_tracking_state(result.detections[0], 42, SSV_TRACK_MATCHED, true);
}

static void test_take_for_tracking_returns_empty_on_wrong_state() {
    auto &store = SsvDetectionStore::instance();
    (void)store.take();
    (void)store.take_for_tracking();

    // EMPTY state → take_for_tracking returns empty
    auto empty1 = store.take_for_tracking();
    assert(empty1.detections.empty());

    // Set to HAS_DETECTIONS, then take_for_tracking → consumes
    store.set(make_detection(130));
    auto consumed = store.take_for_tracking();
    assert(consumed.frame_id == 130);

    // Back to EMPTY, second take_for_tracking returns empty
    auto empty2 = store.take_for_tracking();
    assert(empty2.detections.empty());
}

static void test_take_returns_empty_on_wrong_state() {
    auto &store = SsvDetectionStore::instance();
    (void)store.take();
    (void)store.take_for_tracking();

    // EMPTY state → take returns empty
    auto empty = store.take();
    assert(empty.detections.empty());
}

static void test_set_tracked_filters_invalid_detections() {
    auto &store = SsvDetectionStore::instance();
    (void)store.take();
    (void)store.take_for_tracking();

    SsvFrameDetections frame;
    frame.frame_id = 140;
    std::snprintf(frame.source_id, sizeof(frame.source_id), "track-test");

    // One valid detection with full tracking state
    SsvDetection valid{};
    std::snprintf(valid.class_name, sizeof(valid.class_name), "person");
    valid.confidence = 0.9f;
    valid.x1 = 0.1f; valid.y1 = 0.2f; valid.x2 = 0.3f; valid.y2 = 0.4f;
    valid.class_id = 0;
    valid.track_id = 99;
    valid.track_state = SSV_TRACK_MATCHED;
    valid.occluded = false;

    // One invalid detection (NaN bbox) with track_id
    SsvDetection invalid{};
    std::snprintf(invalid.class_name, sizeof(invalid.class_name), "helmet");
    invalid.confidence = 0.8f;
    invalid.x1 = std::numeric_limits<float>::quiet_NaN();
    invalid.y1 = 0.5f; invalid.x2 = 0.7f; invalid.y2 = 0.8f;
    invalid.class_id = 1;
    invalid.track_id = 77;
    invalid.track_state = SSV_TRACK_NEW;
    invalid.occluded = true;

    frame.detections.push_back(valid);
    frame.detections.push_back(invalid);
    store.set_tracked(std::move(frame));

    auto result = store.take();
    assert(result.frame_id == 140);
    assert(result.detections.size() == 1);
    assert_detection_tracking_state(result.detections[0], 99, SSV_TRACK_MATCHED, false);
}

static void test_set_tracked_with_empty_detections() {
    auto &store = SsvDetectionStore::instance();
    (void)store.take();
    (void)store.take_for_tracking();

    // set_tracked with empty detections list
    SsvFrameDetections frame;
    frame.frame_id = 150;
    std::snprintf(frame.source_id, sizeof(frame.source_id), "empty-track");

    store.set_tracked(std::move(frame));

    // State should be HAS_TRACKS → take() returns the frame
    auto result = store.take();
    assert(result.frame_id == 150);
    assert(result.detections.empty());
}

static void test_overlay_independent_after_take() {
    auto &store = SsvDetectionStore::instance();
    (void)store.take();
    (void)store.take_for_tracking();

    store.set(make_detection(160));
    auto f = store.take_for_tracking();
    f.detections[0].track_id = 88;
    f.detections[0].track_state = SSV_TRACK_MATCHED;
    f.detections[0].occluded = false;
    store.set_tracked(std::move(f));

    // peek_latest before take — tracking state preserved
    auto before = store.peek_latest();
    assert(before.frame_id == 160);
    assert(before.detections.size() == 1);
    assert_detection_tracking_state(before.detections[0], 88, SSV_TRACK_MATCHED, false);

    // take() consumes the data from current_, but not overlay_current_
    auto consumed = store.take();
    assert(consumed.frame_id == 160);
    assert_detection_tracking_state(consumed.detections[0], 88, SSV_TRACK_MATCHED, false);

    // peek_latest still has the overlay copy with tracking state
    auto after = store.peek_latest();
    assert(after.frame_id == 160);
    assert(after.detections.size() == 1);
    assert_detection_tracking_state(after.detections[0], 88, SSV_TRACK_MATCHED, false);
}

static void test_pub_payload_serializes_tracking_metadata() {
    SsvFrameDetections frame;
    frame.frame_id = 170;
    std::snprintf(frame.source_id, sizeof(frame.source_id), "pub-test");

    SsvDetection det{};
    std::snprintf(det.class_name, sizeof(det.class_name), "person");
    det.confidence = 0.72f;
    det.x1 = 0.1f; det.y1 = 0.2f; det.x2 = 0.3f; det.y2 = 0.4f;
    det.class_id = 0;
    det.track_id = 12;
    det.track_state = SSV_TRACK_MATCHED;
    det.occluded = true;
    frame.detections.push_back(det);

    auto payload = ssv_pub_build_event_payload(frame, 1234567890LL);
    auto msg = nlohmann::json::parse(payload);

    assert(msg["type"] == "detection");
    assert(msg["source"] == "pub-test");
    assert(msg["timestamp_ms"] == 1234567890LL);
    assert(msg["frame_id"] == 170);
    assert(msg["detections"].size() == 1);

    const auto &serialized = msg["detections"][0];
    assert(serialized["track_id"] == 12);
    assert(serialized["track_state"] == SSV_TRACK_MATCHED);
    assert(serialized["occluded"] == true);
}

static void test_overlay_tracking_label_glyphs_supported() {
    const char *label = "person #5[No] 0.92";
    for (const char *p = label; *p; ++p) {
        assert(ssv_overlay_glyph_is_supported(*p));
    }
}

void run_ssv_meta_tests() {
    // M1/B tests (detection normalization)
    test_detection_normalizes_valid_detection();
    test_detection_clamps_slightly_out_of_bounds_bbox();
    test_detection_rejects_invalid_numbers();
    test_detection_rejects_empty_bbox_after_clamp();
    test_detection_normalizes_invalid_negative_ids();
    test_detection_normalizes_invalid_track_state();
    test_detection_store_tracking_flow();
    test_detection_store_overwrites_stale_detection();
    test_detection_store_filters_invalid_detection();

    // M2/B tests (tracking metadata contract)
    test_track_id_write_back_multiple_detections();
    test_track_id_default_preserved();
    test_store_skips_set_when_has_tracks();
    test_take_for_tracking_returns_empty_on_wrong_state();
    test_take_returns_empty_on_wrong_state();
    test_set_tracked_filters_invalid_detections();
    test_set_tracked_with_empty_detections();
    test_overlay_independent_after_take();
    test_pub_payload_serializes_tracking_metadata();
    test_overlay_tracking_label_glyphs_supported();
}
