#pragma once

#include <array>
#include <vector>

namespace botsort {

struct KalmanState {
    std::array<float, 8> mean{};
    std::array<float, 64> covariance{};
};

class BoTSortKalman {
public:
    KalmanState initiate(float cx, float cy, float w, float h) const;
    KalmanState predict(const KalmanState &state) const;
    KalmanState update(const KalmanState &state, float cx, float cy, float w, float h) const;
};

}  // namespace botsort
