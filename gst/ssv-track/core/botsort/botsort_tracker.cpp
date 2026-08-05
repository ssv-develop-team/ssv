#include "botsort_tracker.hpp"

#include <algorithm>
#include <cmath>

namespace botsort {
namespace {

std::array<float, 4> to_bbox(const KalmanState &state) {
    return {
        state.mean[0] - state.mean[2] * 0.5F,
        state.mean[1] - state.mean[3] * 0.5F,
        state.mean[0] + state.mean[2] * 0.5F,
        state.mean[1] + state.mean[3] * 0.5F,
    };
}

Detection make_detection_from_state(const Detection &det, const KalmanState &state) {
    auto out = det;
    const auto bbox = to_bbox(state);
    out.x1 = bbox[0];
    out.y1 = bbox[1];
    out.x2 = bbox[2];
    out.y2 = bbox[3];
    return out;
}

}  // namespace

BoTSortTracker::BoTSortTracker(TrackerConfig config)
    : config_(config), gmc_(std::make_unique<BoTSortGmc>(config.gmc_method, config.gmc_downscale)) {}

std::array<float, 4>
BoTSortTracker::to_xywh(const Detection &det) {
    const float w = std::max(1e-3F, det.x2 - det.x1);
    const float h = std::max(1e-3F, det.y2 - det.y1);
    return {det.x1 + w * 0.5F, det.y1 + h * 0.5F, w, h};
}

Detection
BoTSortTracker::sync_detection_from_state(const Detection &det, const KalmanState &kf_state) {
    return make_detection_from_state(det, kf_state);
}

KalmanState
BoTSortTracker::apply_gmc_to_state(const KalmanState &state, const GmcWarp &warp) {
    KalmanState out{};
    for (int group = 0; group < 4; ++group) {
        const int i = group * 2;
        out.mean[i] = static_cast<float>(warp.m00 * state.mean[i] + warp.m01 * state.mean[i + 1]);
        out.mean[i + 1] = static_cast<float>(warp.m10 * state.mean[i] + warp.m11 * state.mean[i + 1]);
        if (group == 0) {
            out.mean[i] += static_cast<float>(warp.m02);
            out.mean[i + 1] += static_cast<float>(warp.m12);
        }
    }
    auto transform = [&warp](int out_index, int in_index) {
        if (out_index / 2 != in_index / 2) return 0.0;
        const bool out_x = (out_index % 2) == 0;
        const bool in_x = (in_index % 2) == 0;
        if (out_x && in_x) return warp.m00;
        if (out_x && !in_x) return warp.m01;
        if (!out_x && in_x) return warp.m10;
        return warp.m11;
    };
    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 8; ++c) {
            double value = 0.0;
            for (int a = 0; a < 8; ++a) {
                for (int b = 0; b < 8; ++b) {
                    value += transform(r, a) * state.covariance[a * 8 + b] * transform(c, b);
                }
            }
            out.covariance[r * 8 + c] = static_cast<float>(value);
        }
    }
    return out;
}

BoTSortTracker::TrackRecord
BoTSortTracker::warp_record(const TrackRecord &rec, const GmcWarp &warp) {
    auto out = rec;
    if (!warp.is_identity()) {
        out.kf_state = apply_gmc_to_state(out.kf_state, warp);
    }
    out.det = sync_detection_from_state(out.det, out.kf_state);
    return out;
}

float
BoTSortTracker::compute_cost(const Detection &track_det, const Detection &det, const TrackerConfig &config) {
    float cost = iou_cost(track_det, det);
    if (config.enable_score_fuse) {
        cost = fuse_score(cost, det.score);
    }
    return cost;
}

int
BoTSortTracker::max_time_lost(const TrackerConfig &config) {
    const int frame_rate = std::max(1, config.frame_rate);
    const int buffer = std::max(1, config.track_buffer);
    return std::max(1, (buffer * frame_rate) / 30);
}

void
BoTSortTracker::remove_duplicates(std::vector<TrackRecord> &tracked_tracks, std::vector<TrackRecord> &lost_tracks) {
    std::vector<int> drop_tracked;
    std::vector<int> drop_lost;
    for (std::size_t i = 0; i < tracked_tracks.size(); ++i) {
        for (std::size_t j = 0; j < lost_tracks.size(); ++j) {
            if (iou_cost(tracked_tracks[i].det, lost_tracks[j].det) >= 0.15F) continue;
            const int time_tracked = tracked_tracks[i].last_frame - tracked_tracks[i].start_frame;
            const int time_lost = lost_tracks[j].last_frame - lost_tracks[j].start_frame;
            if (time_tracked > time_lost) {
                drop_lost.push_back(static_cast<int>(j));
            } else {
                drop_tracked.push_back(static_cast<int>(i));
            }
        }
    }
    std::sort(drop_tracked.begin(), drop_tracked.end());
    drop_tracked.erase(std::unique(drop_tracked.begin(), drop_tracked.end()), drop_tracked.end());
    std::sort(drop_lost.begin(), drop_lost.end());
    drop_lost.erase(std::unique(drop_lost.begin(), drop_lost.end()), drop_lost.end());

    std::vector<TrackRecord> tracked_out;
    tracked_out.reserve(tracked_tracks.size());
    for (std::size_t i = 0; i < tracked_tracks.size(); ++i) {
        if (!std::binary_search(drop_tracked.begin(), drop_tracked.end(), static_cast<int>(i))) {
            tracked_out.push_back(tracked_tracks[i]);
        }
    }
    std::vector<TrackRecord> lost_out;
    lost_out.reserve(lost_tracks.size());
    for (std::size_t i = 0; i < lost_tracks.size(); ++i) {
        if (!std::binary_search(drop_lost.begin(), drop_lost.end(), static_cast<int>(i))) {
            lost_out.push_back(lost_tracks[i]);
        }
    }
    tracked_tracks = std::move(tracked_out);
    lost_tracks = std::move(lost_out);
}

UpdateResult
BoTSortTracker::update(
    const FrameDetections &detections,
    std::optional<GmcFrameView> gmc_frame) {
    ++frame_id_;

    UpdateResult out{};
    out.stats.frame_id = frame_id_;
    out.stats.input_count = static_cast<int>(detections.size());

    GmcWarp warp;
    if (gmc_) {
        const auto *gmc_view = gmc_frame && !gmc_frame->rgba.empty()
            ? &*gmc_frame
            : nullptr;
        warp = gmc_->estimate(gmc_view);
        out.stats.gmc_applied = gmc_view != nullptr
            && config_.gmc_method == GmcMethod::kSparseOptFlow;
        out.stats.gmc_fallback_identity = gmc_->used_fallback_identity();
    }

    std::vector<Detection> high_dets;
    std::vector<Detection> low_dets;
    for (std::size_t input_index = 0; input_index < detections.size(); ++input_index) {
        auto det = detections[input_index];
        det.input_index = static_cast<int>(input_index);
        if (det.x2 <= det.x1 || det.y2 <= det.y1) continue;
        if (det.score <= config_.track_low_thresh) continue;
        if (det.score > config_.track_high_thresh) {
            high_dets.push_back(det);
        } else {
            low_dets.push_back(det);
        }
    }
    out.stats.filtered_count = static_cast<int>(high_dets.size() + low_dets.size());
    out.stats.high_count = static_cast<int>(high_dets.size());
    out.stats.low_count = static_cast<int>(low_dets.size());

    std::vector<TrackRecord> predicted_tracked;
    predicted_tracked.reserve(tracked_.size());
    for (const auto &rec : tracked_) {
        auto pred = rec;
        pred.kf_state = kalman_.predict(pred.kf_state);
        pred.det = sync_detection_from_state(pred.det, pred.kf_state);
        pred = warp_record(pred, warp);
        predicted_tracked.push_back(pred);
    }

    std::vector<TrackRecord> predicted_lost;
    predicted_lost.reserve(lost_.size());
    for (const auto &rec : lost_) {
        auto pred = rec;
        pred.kf_state.mean[6] = 0.0F;
        pred.kf_state.mean[7] = 0.0F;
        pred.kf_state = kalman_.predict(pred.kf_state);
        pred.det = sync_detection_from_state(pred.det, pred.kf_state);
        pred = warp_record(pred, warp);
        predicted_lost.push_back(pred);
    }

    std::vector<TrackRecord> warped_unconfirmed;
    warped_unconfirmed.reserve(unconfirmed_.size());
    for (const auto &rec : unconfirmed_) {
        auto pred = rec;
        pred.det = sync_detection_from_state(pred.det, pred.kf_state);
        pred = warp_record(pred, warp);
        warped_unconfirmed.push_back(pred);
    }

    std::vector<TrackRecord> next_tracked;
    std::vector<TrackRecord> next_lost;
    std::vector<TrackRecord> next_removed = removed_;
    std::vector<bool> high_used(high_dets.size(), false);
    std::vector<bool> tracked_matched(predicted_tracked.size(), false);
    std::vector<bool> lost_matched(predicted_lost.size(), false);
    std::vector<bool> unconfirmed_matched(warped_unconfirmed.size(), false);
    std::vector<Detection> output_detections;

    std::vector<Detection> stage1_tracks;
    std::vector<bool> stage1_from_lost;
    std::vector<int> stage1_orig_index;
    for (std::size_t i = 0; i < predicted_tracked.size(); ++i) {
        stage1_tracks.push_back(predicted_tracked[i].det);
        stage1_from_lost.push_back(false);
        stage1_orig_index.push_back(static_cast<int>(i));
    }
    for (std::size_t i = 0; i < predicted_lost.size(); ++i) {
        stage1_tracks.push_back(predicted_lost[i].det);
        stage1_from_lost.push_back(true);
        stage1_orig_index.push_back(static_cast<int>(i));
    }

    std::vector<std::vector<float>> stage1_cost(stage1_tracks.size(), std::vector<float>(high_dets.size(), 1.0F));
    for (std::size_t ti = 0; ti < stage1_tracks.size(); ++ti) {
        for (std::size_t di = 0; di < high_dets.size(); ++di) {
            const Detection &ref = stage1_from_lost[ti] ? predicted_lost[stage1_orig_index[ti]].det : predicted_tracked[stage1_orig_index[ti]].det;
            if (config_.enable_class_constraint && !classes_compatible(ref.class_id, high_dets[di].class_id)) {
                stage1_cost[ti][di] = 2.0F;
                continue;
            }
            stage1_cost[ti][di] = compute_cost(stage1_tracks[ti], high_dets[di], config_);
        }
    }

    const auto stage1 = linear_assignment(stage1_cost, config_.match_thresh);
    for (const auto &match : stage1.matches) {
        const int ti = match.first;
        const int di = match.second;
        auto det = high_dets[di];
        const bool from_lost = stage1_from_lost[ti];
        auto rec = from_lost ? predicted_lost[stage1_orig_index[ti]] : predicted_tracked[stage1_orig_index[ti]];
        det.track_id = rec.det.track_id;
        det.track_state = TrackState::kMatched;
        det.occluded = det.score < config_.track_thresh;
        rec.kf_state = kalman_.update(rec.kf_state, to_xywh(det)[0], to_xywh(det)[1], to_xywh(det)[2], to_xywh(det)[3]);
        rec.det = det;
        rec.last_frame = frame_id_;
        next_tracked.push_back(rec);
        output_detections.push_back(sync_detection_from_state(det, rec.kf_state));
        high_used[di] = true;
        if (from_lost) {
            lost_matched[stage1_orig_index[ti]] = true;
        } else {
            tracked_matched[stage1_orig_index[ti]] = true;
        }
        ++out.stats.matched_first_stage;
    }

    std::vector<int> second_track_indices;
    for (std::size_t i = 0; i < predicted_tracked.size(); ++i) {
        if (!tracked_matched[i]) second_track_indices.push_back(static_cast<int>(i));
    }
    std::vector<int> low_indices;
    for (std::size_t di = 0; di < low_dets.size(); ++di) low_indices.push_back(static_cast<int>(di));

    std::vector<std::vector<float>> stage2_cost(second_track_indices.size(), std::vector<float>(low_indices.size(), 1.0F));
    for (std::size_t r = 0; r < second_track_indices.size(); ++r) {
        const int ti = second_track_indices[r];
        for (std::size_t c = 0; c < low_indices.size(); ++c) {
            const int di = low_indices[c];
            if (config_.enable_class_constraint && !classes_compatible(predicted_tracked[ti].det.class_id, low_dets[di].class_id)) {
                stage2_cost[r][c] = 2.0F;
                continue;
            }
            stage2_cost[r][c] = iou_cost(predicted_tracked[ti].det, low_dets[di]);
        }
    }

    const auto stage2 = linear_assignment(stage2_cost, 0.5F);
    for (const auto &match : stage2.matches) {
        const int ti = second_track_indices[match.first];
        const int di = low_indices[match.second];
        auto det = low_dets[di];
        auto rec = predicted_tracked[ti];
        det.track_id = rec.det.track_id;
        det.track_state = TrackState::kMatched;
        det.occluded = det.score < config_.track_thresh;
        rec.kf_state = kalman_.update(rec.kf_state, to_xywh(det)[0], to_xywh(det)[1], to_xywh(det)[2], to_xywh(det)[3]);
        rec.det = det;
        rec.last_frame = frame_id_;
        next_tracked.push_back(rec);
        output_detections.push_back(sync_detection_from_state(det, rec.kf_state));
        tracked_matched[ti] = true;
        ++out.stats.matched_second_stage;
    }

    std::vector<int> unconfirmed_indices;
    for (std::size_t i = 0; i < warped_unconfirmed.size(); ++i) {
        unconfirmed_indices.push_back(static_cast<int>(i));
    }
    std::vector<int> remaining_high_indices;
    for (std::size_t di = 0; di < high_dets.size(); ++di) {
        if (!high_used[di]) remaining_high_indices.push_back(static_cast<int>(di));
    }

    std::vector<std::vector<float>> stage3_cost(unconfirmed_indices.size(), std::vector<float>(remaining_high_indices.size(), 1.0F));
    for (std::size_t r = 0; r < unconfirmed_indices.size(); ++r) {
        const int ui = unconfirmed_indices[r];
        for (std::size_t c = 0; c < remaining_high_indices.size(); ++c) {
            const int di = remaining_high_indices[c];
            if (config_.enable_class_constraint && !classes_compatible(warped_unconfirmed[ui].det.class_id, high_dets[di].class_id)) {
                stage3_cost[r][c] = 2.0F;
                continue;
            }
            stage3_cost[r][c] = compute_cost(warped_unconfirmed[ui].det, high_dets[di], config_);
        }
    }

    const auto stage3 = linear_assignment(stage3_cost, 0.7F);
    for (const auto &match : stage3.matches) {
        const int ui = unconfirmed_indices[match.first];
        const int di = remaining_high_indices[match.second];
        auto det = high_dets[di];
        auto rec = warped_unconfirmed[ui];
        det.track_id = rec.det.track_id;
        det.track_state = TrackState::kMatched;
        det.occluded = det.score < config_.track_thresh;
        rec.kf_state = kalman_.update(rec.kf_state, to_xywh(det)[0], to_xywh(det)[1], to_xywh(det)[2], to_xywh(det)[3]);
        rec.det = det;
        rec.last_frame = frame_id_;
        next_tracked.push_back(rec);
        output_detections.push_back(sync_detection_from_state(det, rec.kf_state));
        high_used[di] = true;
        unconfirmed_matched[ui] = true;
    }

    for (std::size_t i = 0; i < warped_unconfirmed.size(); ++i) {
        if (!unconfirmed_matched[i]) {
            next_removed.push_back(warped_unconfirmed[i]);
            ++out.stats.removed_tracks;
        }
    }

    for (std::size_t i = 0; i < predicted_tracked.size(); ++i) {
        if (!tracked_matched[i]) {
            next_lost.push_back(predicted_tracked[i]);
            ++out.stats.lost_tracks;
        }
    }

    const int ttl = max_time_lost(config_);
    for (std::size_t i = 0; i < predicted_lost.size(); ++i) {
        if (lost_matched[i]) continue;
        if (frame_id_ - predicted_lost[i].last_frame > ttl) {
            next_removed.push_back(predicted_lost[i]);
            ++out.stats.removed_tracks;
            continue;
        }
        next_lost.push_back(predicted_lost[i]);
    }

    std::vector<TrackRecord> next_unconfirmed;
    for (std::size_t di = 0; di < high_dets.size(); ++di) {
        if (high_used[di]) continue;
        auto det = high_dets[di];
        if (det.score < config_.new_track_thresh) continue;
        det.track_id = next_track_id_++;
        det.track_state = TrackState::kNew;
        det.occluded = det.score < config_.track_thresh;
        TrackRecord rec;
        rec.det = det;
        rec.kf_state = kalman_.initiate(to_xywh(det)[0], to_xywh(det)[1], to_xywh(det)[2], to_xywh(det)[3]);
        rec.start_frame = frame_id_;
        rec.last_frame = frame_id_;
        if (frame_id_ == 1) {
            next_tracked.push_back(rec);
        } else {
            next_unconfirmed.push_back(rec);
        }
        output_detections.push_back(det);
        ++out.stats.new_tracks;
    }

    remove_duplicates(next_tracked, next_lost);

    std::sort(output_detections.begin(), output_detections.end(), [](const Detection &lhs, const Detection &rhs) {
        return lhs.input_index < rhs.input_index;
    });
    out.detections = restore_input_order(output_detections, detections.size());
    out.detections.erase(std::remove_if(out.detections.begin(), out.detections.end(), [](const Detection &det) { return det.track_id < 0; }), out.detections.end());

    tracked_ = std::move(next_tracked);
    lost_ = std::move(next_lost);
    unconfirmed_ = std::move(next_unconfirmed);
    removed_ = std::move(next_removed);
    return out;
}

}  // namespace botsort
