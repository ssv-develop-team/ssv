#pragma once

#include <gst/gst.h>

#include <mutex>
#include <string>
#include <vector>

/// Single detection result from inference.
struct SsvDetection {
    char class_name[32];   ///< e.g. "person"
    float confidence;      ///< [0, 1]
    float x1, y1, x2, y2; ///< bounding box in normalized [0, 1] coordinates
    int class_id = -1;     ///< YOLO class index (0=person, etc.), -1 = unset
    int track_id = -1;     ///< assigned by ssvtrack, -1 = not tracked
};

/// Per-frame detection result.
struct SsvFrameDetections {
    guint64 frame_id = 0;
    char source_id[64] = {};
    std::vector<SsvDetection> detections;
};

struct SsvOverlayFrame {
    guint64 frame_id = 0;
    char source_id[64] = {};
    std::vector<SsvDetection> detections;
};

/// Thread-safe singleton for passing detections between plugins.
///
/// Three-state model (M3):
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
