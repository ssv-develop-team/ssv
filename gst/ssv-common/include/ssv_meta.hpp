#pragma once

#include <gst/gst.h>

#include <mutex>
#include <string>
#include <vector>

/// Single detection result in original-frame normalized coordinates.
struct SsvDetection {
    char class_name[32];   ///< Short label name, e.g. "person"
    float confidence;      ///< finite [0, 1]
    float x1, y1, x2, y2; ///< original-frame normalized bbox, [0, 1], left-top/right-bottom
    int class_id = -1;     ///< model class index, -1 = unset
    int track_id = -1;     ///< assigned by ssvtrack, -1 = not tracked
};

/// Per-frame detection result.
struct SsvFrameDetections {
    guint64 frame_id = 0;
    char source_id[64] = {};
    std::vector<SsvDetection> detections;
};

/// Normalizes one detection in-place.
///
/// Returns false when the detection violates the public metadata contract and
/// must be dropped before reaching tracking, overlay, or publishing.
bool ssv_normalize_detection(SsvDetection &det);

struct SsvOverlayFrame {
    guint64 frame_id = 0;
    char source_id[64] = {};
    std::vector<SsvDetection> detections;
};

/// Thread-safe singleton for passing detections between plugins.
///
/// Three-state model for inference, tracking, publishing, and overlay:
///   EMPTY ──set()──→ HAS_DETECTIONS ──take_for_tracking()──→ EMPTY
///     ↑                                        │
///     │                           set_tracked()│
///     │                                        ↓
///     └──────── take() ←─── HAS_TRACKS ←──────┘
///
/// ssvinfer calls set(), ssvtrack calls take_for_tracking()/set_tracked(),
/// ssvpub calls take().
class SsvDetectionStore {
public:
    static SsvDetectionStore &instance();

    /// Called by ssvinfer.  Overwrites EMPTY or HAS_DETECTIONS (stale).
    /// Skips if HAS_TRACKS (unpublished data waiting for ssvpub).
    void set(SsvFrameDetections det);

    /// Called by ssvtrack.  Returns data only when HAS_DETECTIONS.
    SsvFrameDetections take_for_tracking();

    /// Called by ssvtrack.  Writes tracked results.
    void set_tracked(SsvFrameDetections det);

    /// Called by ssvpub.  Returns data only when HAS_TRACKS.
    SsvFrameDetections take();

    /// Called by ssvoverlay.  Returns the latest tracked result without consuming it.
    SsvOverlayFrame peek_latest();

private:
    enum class State { EMPTY, HAS_DETECTIONS, HAS_TRACKS };

    std::mutex mtx_;
    SsvFrameDetections current_;
    SsvOverlayFrame overlay_current_;
    State state_ = State::EMPTY;
};
