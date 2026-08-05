#pragma once

#include "botsort_gmc.hpp"
#include "botsort_kalman.hpp"
#include "botsort_matching.hpp"
#include "botsort_types.hpp"

#include <memory>
#include <optional>
#include <vector>

namespace botsort {

class BoTSortTracker {
public:
    explicit BoTSortTracker(TrackerConfig config);

    UpdateResult update(
        const FrameDetections &detections,
        std::optional<GmcFrameView> gmc_frame = std::nullopt);

    static KalmanState apply_gmc_to_state(const KalmanState &state, const GmcWarp &warp);

private:
    struct TrackRecord {
        Detection det;
        KalmanState kf_state{};
        int start_frame = 0;
        int last_frame = 0;
    };

    static std::array<float, 4> to_xywh(const Detection &det);
    static Detection sync_detection_from_state(const Detection &det, const KalmanState &kf_state);
    static TrackRecord warp_record(const TrackRecord &rec, const GmcWarp &warp);
    static float compute_cost(const Detection &track_det, const Detection &det, const TrackerConfig &config);
    static int max_time_lost(const TrackerConfig &config);
    static void remove_duplicates(std::vector<TrackRecord> &tracked_tracks, std::vector<TrackRecord> &lost_tracks);

    TrackerConfig config_;
    int frame_id_ = 0;
    int next_track_id_ = 1;
    std::vector<TrackRecord> tracked_;
    std::vector<TrackRecord> lost_;
    std::vector<TrackRecord> unconfirmed_;
    std::vector<TrackRecord> removed_;
    std::unique_ptr<BoTSortGmc> gmc_;
    BoTSortKalman kalman_;
};

}  // namespace botsort
