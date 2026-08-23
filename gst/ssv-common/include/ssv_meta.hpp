#pragma once

#include <gst/gst.h>
#include <gst/video/video.h>

#include "ssv_config.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

enum SsvTrackState : int {
    SSV_TRACK_NEW = 0,
    SSV_TRACK_MATCHED = 1,
    SSV_TRACK_LOST = 2,
    SSV_TRACK_DEAD = 3,
};

struct SsvFrameTiming {
    GstClockTime pts = GST_CLOCK_TIME_NONE;
    GstClockTime duration = GST_CLOCK_TIME_NONE;
    std::uint64_t generation = 0;
};

struct PreprocessTransform {
    int source_width = 0;
    int source_height = 0;
    int model_width = 0;
    int model_height = 0;
    float scale_x = 0.0F;
    float scale_y = 0.0F;
    int pad_left = 0;
    int pad_top = 0;
    int pad_right = 0;
    int pad_bottom = 0;

    bool operator==(const PreprocessTransform &) const = default;
};

[[nodiscard]] PreprocessTransform ssv_make_resize_transform(
    int source_width,
    int source_height,
    int model_width,
    int model_height,
    ssv::SsvResizeMode resize_mode);

[[nodiscard]] PreprocessTransform ssv_make_letterbox_transform(
    int source_width,
    int source_height,
    int model_width,
    int model_height);

struct SsvRgbaFrameView {
    std::span<const std::uint8_t> bytes;
    int width = 0;
    int height = 0;
    std::size_t stride = 0;
};

class SsvAnalysisFramePool;

class SsvAnalysisFrame final {
public:
    ~SsvAnalysisFrame();

    SsvAnalysisFrame(const SsvAnalysisFrame &) = delete;
    SsvAnalysisFrame &operator=(const SsvAnalysisFrame &) = delete;

    [[nodiscard]] const SsvRgbaFrameView &view() const noexcept;
    [[nodiscard]] const PreprocessTransform &transform() const noexcept;
    [[nodiscard]] const SsvFrameTiming &timing() const noexcept;

private:
    friend class SsvAnalysisFramePool;

    struct Impl;
    explicit SsvAnalysisFrame(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

struct SsvAnalysisFramePoolStats {
    std::uint64_t map_count = 0;
    std::uint64_t direct_frames = 0;
    std::uint64_t staged_frames = 0;
    std::uint64_t staging_exhaustions = 0;
    std::size_t active_maps = 0;
    std::size_t outstanding_staging_leases = 0;
    std::size_t peak_active_maps = 0;
    std::size_t peak_staging_leases = 0;
};

class SsvAnalysisFramePool final {
public:
    SsvAnalysisFramePool(
        int model_width,
        int model_height,
        std::size_t staging_capacity);
    ~SsvAnalysisFramePool();

    SsvAnalysisFramePool(const SsvAnalysisFramePool &) = delete;
    SsvAnalysisFramePool &operator=(const SsvAnalysisFramePool &) = delete;

    [[nodiscard]] std::shared_ptr<const SsvAnalysisFrame> create(
        GstBuffer *buffer,
        const GstVideoInfo &video_info,
        PreprocessTransform transform,
        SsvFrameTiming timing);
    [[nodiscard]] SsvAnalysisFramePoolStats stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

inline bool operator==(const SsvFrameTiming &left, const SsvFrameTiming &right)
{
    return left.pts == right.pts && left.duration == right.duration &&
        left.generation == right.generation;
}

struct SsvDetection {
    char class_name[32] = {};
    float confidence = 0.0F;
    float x1 = 0.0F;
    float y1 = 0.0F;
    float x2 = 0.0F;
    float y2 = 0.0F;
    int class_id = -1;
};

[[nodiscard]] std::optional<SsvDetection> ssv_unmap_model_detection(
    SsvDetection detection,
    const PreprocessTransform &transform);

struct SsvDetectionFrame {
    std::uint64_t frame_id = 0;
    std::string source_id;
    SsvFrameTiming timing;
    std::vector<SsvDetection> detections;
    std::shared_ptr<const SsvAnalysisFrame> analysis_frame;
};

struct SsvTrackedObject {
    SsvDetection detection;
    int track_id = -1;
    SsvTrackState track_state = SSV_TRACK_NEW;
    bool occluded = false;
};

struct SsvTrackedFrame {
    std::uint64_t frame_id = 0;
    std::string source_id;
    SsvFrameTiming timing;
    std::vector<SsvTrackedObject> objects;
};

struct SsvOverlayBox {
    SsvDetection detection;
    int track_id = -1;
    SsvTrackState track_state = SSV_TRACK_NEW;
    bool occluded = false;
    bool predicted = false;
};

struct SsvOverlayFrame {
    std::string source_id;
    SsvFrameTiming observation_timing;
    SsvFrameTiming display_timing;
    std::vector<SsvOverlayBox> boxes;
};

enum class SsvMetaResult {
    Published,
    Consumed,
    Empty,
    NoPts,
    Occupied,
    WrongSource,
    WrongGeneration,
    DuplicatePts,
    StalePts,
};

struct SsvMetaStats {
    std::uint64_t published = 0;
    std::uint64_t consumed = 0;
    std::uint64_t empty = 0;
    std::uint64_t no_pts = 0;
    std::uint64_t occupied = 0;
    std::uint64_t wrong_source = 0;
    std::uint64_t wrong_generation = 0;
    std::uint64_t duplicate_pts = 0;
    std::uint64_t stale_pts = 0;
    std::uint64_t generation_resets = 0;
    std::size_t max_history_depth = 0;
};

struct SsvDetectionConsumeResult {
    SsvMetaResult result = SsvMetaResult::Empty;
    std::optional<SsvDetectionFrame> frame;
};

struct SsvTrackedConsumeResult {
    SsvMetaResult result = SsvMetaResult::Empty;
    std::shared_ptr<const SsvTrackedFrame> frame;
};

struct SsvTimelineSegment {
    GstClockTime start = 0;
    GstClockTime time = 0;
    GstClockTime base = 0;
    double rate = 1.0;

    bool operator==(const SsvTimelineSegment &) const = default;
};

struct SsvTimelineUpdate {
    std::uint64_t generation = 0;
    bool reset = false;
};

class SsvTimelineCursor;

class SsvSourceMeta final {
public:
    explicit SsvSourceMeta(std::string_view source_id);
    ~SsvSourceMeta();

    SsvSourceMeta(const SsvSourceMeta &) = delete;
    SsvSourceMeta &operator=(const SsvSourceMeta &) = delete;

    /// The immutable view remains valid for this object's lifetime.
    [[nodiscard]] std::string_view source_id() const noexcept;
    std::uint64_t generation() const;

    SsvMetaResult publish_detection(SsvDetectionFrame &&frame);
    SsvDetectionConsumeResult consume_detection();

    SsvMetaResult publish_tracked(
        SsvDetectionFrame &&observation,
        std::vector<SsvTrackedObject> objects);
    SsvTrackedConsumeResult consume_tracked();

    std::shared_ptr<const SsvTrackedFrame> latest_tracked_at_or_before(
        GstClockTime display_pts) const;
    std::size_t history_depth() const;
    SsvMetaStats stats() const;

private:
    friend class SsvTimelineCursor;

    struct Impl;

    std::uint64_t observe_segment(
        const SsvTimelineSegment &segment,
        std::uint64_t expected_generation,
        bool coalesce_reset);
    std::uint64_t request_reset(std::uint64_t expected_generation);

    std::unique_ptr<Impl> impl_;
};

/// Shared identity and metadata owner for one pipeline source.
///
/// Production pipeline construction creates one context and passes its
/// borrowed address to the GStreamer adapters.  The compatibility
/// `ssv_meta(source_id)` facade remains available for standalone callers and
/// tests; constructing a context through it therefore preserves identity
/// with those callers.
class SsvSourceContext final {
public:
    explicit SsvSourceContext(std::string_view source_id);

    [[nodiscard]] std::shared_ptr<SsvSourceMeta> meta() const;
    [[nodiscard]] std::string_view source_id() const noexcept;

private:
    std::shared_ptr<SsvSourceMeta> meta_;
};

class SsvTimelineCursor final {
public:
    explicit SsvTimelineCursor(std::shared_ptr<SsvSourceMeta> source);

    SsvTimelineUpdate on_segment(const SsvTimelineSegment &segment);
    SsvTimelineUpdate on_flush_stop(bool reset_time);
    SsvTimelineUpdate on_buffer(GstClockTime pts, bool discontinuity);
    SsvTimelineUpdate on_lifecycle_reset();

    std::uint64_t generation() const { return generation_; }

private:
    enum class ResetKind {
        None,
        Flush,
        Discontinuity,
        PtsRollback,
        Lifecycle,
    };

    SsvTimelineUpdate synchronize();
    SsvTimelineUpdate reset_once(ResetKind kind);

    std::shared_ptr<SsvSourceMeta> source_;
    std::uint64_t generation_ = 0;
    GstClockTime last_pts_ = GST_CLOCK_TIME_NONE;
    ResetKind last_reset_kind_ = ResetKind::None;
};

std::shared_ptr<SsvSourceMeta> ssv_meta(std::string_view source_id);
