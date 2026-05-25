#include "gstssvtrack.hpp"
#include "ssv_logging.hpp"
#include "ssv_meta.hpp"

#include <gst/video/video.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

GST_DEBUG_CATEGORY_STATIC(ssv_track_debug);

// ── IoU Tracker (self-contained, no external deps) ────────────────────

struct IoUTrack {
    int id = 0;
    float x1 = 0, y1 = 0, x2 = 0, y2 = 0;  // normalized [0,1]
    int class_id = -1;
    int age = 0;              // total matched frames
    int time_since_seen = 0;  // frames since last match
    float dx = 0, dy = 0;    // centroid velocity (for prediction)
    bool alive = true;
};

static float
compute_iou(float ax1, float ay1, float ax2, float ay2,
            float bx1, float by1, float bx2, float by2)
{
    float ix1 = std::max(ax1, bx1);
    float iy1 = std::max(ay1, by1);
    float ix2 = std::min(ax2, bx2);
    float iy2 = std::min(ay2, by2);

    float iw = std::max(0.0f, ix2 - ix1);
    float ih = std::max(0.0f, iy2 - iy1);
    float inter = iw * ih;

    float area_a = (ax2 - ax1) * (ay2 - ay1);
    float area_b = (bx2 - bx1) * (by2 - by1);
    float uni = area_a + area_b - inter;

    return (uni > 0.0f) ? inter / uni : 0.0f;
}

static void
centroid(float x1, float y1, float x2, float y2, float &cx, float &cy) {
    cx = (x1 + x2) * 0.5f;
    cy = (y1 + y2) * 0.5f;
}

class IoUTracker {
public:
    IoUTracker(int max_age, float iou_thresh)
        : max_age_(max_age), iou_thresh_(iou_thresh) {}

    void update(std::vector<SsvDetection> &dets) {
        // Predict positions using velocity
        for (auto &t : tracks_) {
            if (t.alive) {
                float cx, cy;
                centroid(t.x1, t.y1, t.x2, t.y2, cx, cy);
                cx += t.dx;
                cy += t.dy;
                float w = (t.x2 - t.x1) * 0.5f;
                float h = (t.y2 - t.y1) * 0.5f;
                t.x1 = cx - w;
                t.y1 = cy - h;
                t.x2 = cx + w;
                t.y2 = cy + h;
            }
        }

        // Build IoU cost matrix and greedy-match by confidence (descending)
        std::vector<int> det_order(dets.size());
        for (size_t i = 0; i < dets.size(); ++i) det_order[i] = (int)i;
        std::sort(det_order.begin(), det_order.end(), [&](int a, int b) {
            return dets[a].confidence > dets[b].confidence;
        });

        std::vector<bool> track_matched(tracks_.size(), false);
        std::vector<bool> det_matched(dets.size(), false);

        for (int di : det_order) {
            float best_iou = 0.0f;
            int best_ti = -1;
            for (size_t ti = 0; ti < tracks_.size(); ++ti) {
                if (track_matched[ti] || !tracks_[ti].alive)
                    continue;
                if (tracks_[ti].class_id != dets[di].class_id &&
                    tracks_[ti].class_id >= 0 && dets[di].class_id >= 0)
                    continue;
                float iou = compute_iou(
                    tracks_[ti].x1, tracks_[ti].y1, tracks_[ti].x2, tracks_[ti].y2,
                    dets[di].x1, dets[di].y1, dets[di].x2, dets[di].y2);
                if (iou > best_iou) {
                    best_iou = iou;
                    best_ti = (int)ti;
                }
            }
            if (best_ti >= 0 && best_iou >= iou_thresh_) {
                auto &t = tracks_[best_ti];
                // Compute velocity before updating bbox
                float old_cx, old_cy, new_cx, new_cy;
                centroid(t.x1, t.y1, t.x2, t.y2, old_cx, old_cy);
                centroid(dets[di].x1, dets[di].y1, dets[di].x2, dets[di].y2, new_cx, new_cy);
                t.dx = new_cx - old_cx;
                t.dy = new_cy - old_cy;
                t.x1 = dets[di].x1;
                t.y1 = dets[di].y1;
                t.x2 = dets[di].x2;
                t.y2 = dets[di].y2;
                t.age++;
                t.time_since_seen = 0;
                dets[di].track_id = t.id;
                track_matched[best_ti] = true;
                det_matched[di] = true;
            }
        }

        // Unmatched tracks: increment time_since_seen
        for (size_t ti = 0; ti < tracks_.size(); ++ti) {
            if (!track_matched[ti] && tracks_[ti].alive) {
                tracks_[ti].time_since_seen++;
                if (tracks_[ti].time_since_seen > max_age_) {
                    tracks_[ti].alive = false;
                }
            }
        }

        // Unmatched detections: create new tracks
        for (size_t di = 0; di < dets.size(); ++di) {
            if (!det_matched[di]) {
                IoUTrack t;
                t.id = next_id_++;
                t.x1 = dets[di].x1;
                t.y1 = dets[di].y1;
                t.x2 = dets[di].x2;
                t.y2 = dets[di].y2;
                t.class_id = dets[di].class_id;
                t.age = 1;
                t.time_since_seen = 0;
                t.alive = true;
                dets[di].track_id = t.id;
                tracks_.push_back(t);
            }
        }

        // Remove dead tracks
        tracks_.erase(
            std::remove_if(tracks_.begin(), tracks_.end(),
                           [](const IoUTrack &t) { return !t.alive; }),
            tracks_.end());
    }

private:
    int next_id_ = 1;
    int max_age_;
    float iou_thresh_;
    std::vector<IoUTrack> tracks_;
};

// ── GObject struct ─────────────────────────────────────────────────────
struct _SsvTrack {
    GstBaseTransform parent;

    gint frame_rate;
    gfloat track_thresh;
    gint track_buffer;
    gfloat match_thresh;
    gboolean mock_track;

    IoUTracker *tracker;
    gint mock_next_id;
};

enum {
    PROP_0,
    PROP_FRAME_RATE,
    PROP_TRACK_THRESH,
    PROP_TRACK_BUFFER,
    PROP_MATCH_THRESH,
    PROP_MOCK_TRACK,
};

G_DEFINE_TYPE(SsvTrack, ssv_track, GST_TYPE_BASE_TRANSFORM)

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE(
    "sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS(
        "video/x-raw, format=(string)BGR, "
        "width=(int)[1, MAX], height=(int)[1, MAX], "
        "framerate=(fraction)[0, MAX]"));

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
    "src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS(
        "video/x-raw, format=(string)BGR, "
        "width=(int)[1, MAX], height=(int)[1, MAX], "
        "framerate=(fraction)[0, MAX]"));

// ── GstBaseTransform callbacks ────────────────────────────────────────

static gboolean
ssv_track_start(GstBaseTransform *trans) {
    auto *self = SSV_TRACK(trans);

    if (self->mock_track) {
        self->mock_next_id = 1;
        GST_INFO_OBJECT(self, "mock-track enabled (sequential IDs)");
    } else {
        self->tracker = new IoUTracker(self->track_buffer, self->match_thresh);
        GST_INFO_OBJECT(self, "IoU tracker started (buffer=%d, match_thresh=%.2f)",
            self->track_buffer, self->match_thresh);
    }
    return TRUE;
}

static gboolean
ssv_track_stop(GstBaseTransform *trans) {
    auto *self = SSV_TRACK(trans);
    delete self->tracker;
    self->tracker = nullptr;
    return TRUE;
}

static GstFlowReturn
ssv_track_transform_ip(GstBaseTransform *trans, GstBuffer *buf) {
    (void)buf;
    auto *self = SSV_TRACK(trans);

    auto det = SsvDetectionStore::instance().take_for_tracking();
    if (det.detections.empty() && det.frame_id == 0) {
        // No fresh detections, pass frame through
        return GST_FLOW_OK;
    }

    if (self->mock_track) {
        // Mock mode: assign sequential IDs (one per detection per frame)
        for (auto &d : det.detections) {
            d.track_id = self->mock_next_id++;
        }
    } else if (self->tracker) {
        self->tracker->update(det.detections);
    }

    if (!det.detections.empty()) {
        GST_DEBUG_OBJECT(self, "frame %" G_GUINT64_FORMAT ": %zu tracked detections",
            det.frame_id, det.detections.size());
    }

    SsvDetectionStore::instance().set_tracked(std::move(det));
    return GST_FLOW_OK;
}

// ── Properties ─────────────────────────────────────────────────────────

static void
ssv_track_set_property(GObject *object, guint prop_id,
                        const GValue *value, GParamSpec *pspec) {
    auto *self = SSV_TRACK(object);
    switch (prop_id) {
    case PROP_FRAME_RATE:
        self->frame_rate = g_value_get_int(value);
        break;
    case PROP_TRACK_THRESH:
        self->track_thresh = g_value_get_float(value);
        break;
    case PROP_TRACK_BUFFER:
        self->track_buffer = g_value_get_int(value);
        break;
    case PROP_MATCH_THRESH:
        self->match_thresh = g_value_get_float(value);
        break;
    case PROP_MOCK_TRACK:
        self->mock_track = g_value_get_boolean(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void
ssv_track_get_property(GObject *object, guint prop_id,
                        GValue *value, GParamSpec *pspec) {
    auto *self = SSV_TRACK(object);
    switch (prop_id) {
    case PROP_FRAME_RATE:
        g_value_set_int(value, self->frame_rate);
        break;
    case PROP_TRACK_THRESH:
        g_value_set_float(value, self->track_thresh);
        break;
    case PROP_TRACK_BUFFER:
        g_value_set_int(value, self->track_buffer);
        break;
    case PROP_MATCH_THRESH:
        g_value_set_float(value, self->match_thresh);
        break;
    case PROP_MOCK_TRACK:
        g_value_set_boolean(value, self->mock_track);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

// ── Class / instance init ──────────────────────────────────────────────

static void
ssv_track_finalize(GObject *object) {
    auto *self = SSV_TRACK(object);
    delete self->tracker;
    self->tracker = nullptr;
    G_OBJECT_CLASS(ssv_track_parent_class)->finalize(object);
}

static void
ssv_track_class_init(SsvTrackClass *klass) {
    auto *gobject_class = G_OBJECT_CLASS(klass);
    auto *base_class = GST_BASE_TRANSFORM_CLASS(klass);
    auto *element_class = GST_ELEMENT_CLASS(klass);

    gobject_class->set_property = ssv_track_set_property;
    gobject_class->get_property = ssv_track_get_property;
    gobject_class->finalize = ssv_track_finalize;

    g_object_class_install_property(gobject_class, PROP_FRAME_RATE,
        g_param_spec_int("frame-rate", "Frame Rate",
            "Pipeline frame rate",
            1, 120, 30,
            (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_TRACK_THRESH,
        g_param_spec_float("track-thresh", "Track Threshold",
            "Tracking confidence threshold",
            0.0f, 1.0f, 0.5f,
            (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_TRACK_BUFFER,
        g_param_spec_int("track-buffer", "Track Buffer",
            "Frames to retain lost tracks",
            1, 300, 30,
            (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_MATCH_THRESH,
        g_param_spec_float("match-thresh", "Match Threshold",
            "IoU matching threshold",
            0.0f, 1.0f, 0.3f,
            (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_MOCK_TRACK,
        g_param_spec_boolean("mock-track", "Mock Track",
            "Assign sequential IDs without real tracking",
            FALSE, (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    gst_element_class_set_static_metadata(element_class,
        "SSV IoU Tracker",
        "Filter/Effect/Video",
        "Multi-object tracking using IoU-based matching",
        "site-safety-vision");

    gst_element_class_add_static_pad_template(element_class, &sink_template);
    gst_element_class_add_static_pad_template(element_class, &src_template);

    base_class->start = ssv_track_start;
    base_class->stop = ssv_track_stop;
    base_class->transform_ip = ssv_track_transform_ip;
    base_class->passthrough_on_same_caps = TRUE;
}

static void
ssv_track_init(SsvTrack *self) {
    self->frame_rate = 30;
    self->track_thresh = 0.5f;
    self->track_buffer = 30;
    self->match_thresh = 0.3f;
    self->mock_track = FALSE;
    self->tracker = nullptr;
    self->mock_next_id = 1;
}

// ── Plugin registration ────────────────────────────────────────────────

GST_ELEMENT_REGISTER_DEFINE(ssv_track, "ssvtrack",
    GST_RANK_NONE, SSV_TYPE_TRACK)

static gboolean
plugin_init(GstPlugin *plugin) {
    SSV_GST_DEBUG_INIT(ssv_track_debug, "ssv-track");
    return GST_ELEMENT_REGISTER(ssv_track, plugin);
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR, GST_VERSION_MINOR,
    ssvtrack,
    "SSV IoU Tracker Plugin",
    plugin_init,
    "0.1.0", "LGPL",
    "site-safety-vision",
    "https://github.com/site-safety-vision/site-safety-vision")
