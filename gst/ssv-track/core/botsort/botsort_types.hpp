#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace botsort {

enum class GmcMethod : std::uint8_t {
    kNone = 0,
    kSparseOptFlow = 1,
};

enum class TrackState : std::uint8_t {
    kNew = 0,
    kMatched = 1,
    kLost = 2,
    kRemoved = 3,
};

struct Detection {
    float x1 = 0.0F;
    float y1 = 0.0F;
    float x2 = 0.0F;
    float y2 = 0.0F;
    float score = 0.0F;
    int class_id = -1;
    std::string class_name;
    int input_index = -1;
    int track_id = -1;
    TrackState track_state = TrackState::kNew;
    bool occluded = false;
};

using FrameDetections = std::vector<Detection>;

struct GmcFrameView {
    std::span<const std::uint8_t> rgba;
    int model_width = 0;
    int model_height = 0;
    std::size_t rgba_stride = 0;
    float source_to_model_scale = 0.0F;
    int pad_left = 0;
    int pad_top = 0;
};

struct DebugStats {
    int frame_id = 0;
    int input_count = 0;
    int filtered_count = 0;
    int high_count = 0;
    int low_count = 0;
    int matched_first_stage = 0;
    int matched_second_stage = 0;
    int new_tracks = 0;
    int lost_tracks = 0;
    int removed_tracks = 0;
    bool gmc_applied = false;
    bool gmc_fallback_identity = false;
};

struct TrackerConfig {
    float track_low_thresh = 0.1F;
    float track_high_thresh = 0.6F;
    float new_track_thresh = 0.7F;
    float match_thresh = 0.8F;
    float track_thresh = 0.5F;
    int track_buffer = 30;
    int frame_rate = 30;
    int gmc_downscale = 2;
    bool enable_score_fuse = true;
    bool enable_class_constraint = false;
    GmcMethod gmc_method = GmcMethod::kSparseOptFlow;
};

struct UpdateResult {
    FrameDetections detections;
    DebugStats stats;
};

}  // namespace botsort
