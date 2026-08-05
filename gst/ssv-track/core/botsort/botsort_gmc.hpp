#pragma once

#include "botsort_types.hpp"

#include <array>
#include <cstddef>

#ifndef SSV_HAS_OPENCV
#define SSV_HAS_OPENCV 0
#endif

#if SSV_HAS_OPENCV
#include <opencv2/core.hpp>
#include <vector>
#endif

namespace botsort {

bool should_attempt_sparse_opt_flow(std::size_t previous_point_count);
[[nodiscard]] bool gmc_method_available(GmcMethod method) noexcept;

struct GmcWarp {
    double m00 = 1.0;
    double m01 = 0.0;
    double m02 = 0.0;
    double m10 = 0.0;
    double m11 = 1.0;
    double m12 = 0.0;

    bool is_identity() const;
};

[[nodiscard]] GmcWarp gmc_warp_to_source_coordinates(
    const GmcWarp &model_warp,
    float source_to_model_scale,
    int pad_left,
    int pad_top);

class BoTSortGmc {
public:
    BoTSortGmc(GmcMethod method, int downscale);

    GmcWarp estimate(const GmcFrameView *frame);
    void reset();
    bool used_fallback_identity() const;

    static std::array<float, 4> apply_bbox(const GmcWarp &warp, const std::array<float, 4> &bbox);

private:
    static GmcWarp identity();
    GmcWarp estimate_sparse_opt_flow(const GmcFrameView &frame);

    GmcMethod method_;
    int downscale_ = 2;
    bool initialized_ = false;
    bool used_fallback_identity_ = false;
#if SSV_HAS_OPENCV
    cv::Mat prev_gray_;
    std::vector<cv::Point2f> prev_points_;
#endif
};

}  // namespace botsort
