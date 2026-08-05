#include "botsort_kalman.hpp"

#include <algorithm>
#include <cmath>

namespace botsort {
namespace {

constexpr float kStdWeightPosition = 1.0F / 20.0F;
constexpr float kStdWeightVelocity = 1.0F / 160.0F;

using Matrix4 = std::array<float, 16>;
using Matrix8 = std::array<float, 64>;
using Vector4 = std::array<float, 4>;
using Vector8 = std::array<float, 8>;

float clamp_extent(float value) {
    return std::max(value, 1e-3F);
}

Matrix8 identity8() {
    Matrix8 out{};
    for (int i = 0; i < 8; ++i) {
        out[i * 8 + i] = 1.0F;
    }
    return out;
}

Matrix8 diag8(const Vector8 &values) {
    Matrix8 out{};
    for (int i = 0; i < 8; ++i) out[i * 8 + i] = values[i];
    return out;
}

Matrix4 diag4(const Vector4 &values) {
    Matrix4 out{};
    for (int i = 0; i < 4; ++i) out[i * 4 + i] = values[i];
    return out;
}

Vector8 mul(const Matrix8 &a, const Vector8 &x) {
    Vector8 out{};
    for (int r = 0; r < 8; ++r) {
        float sum = 0.0F;
        for (int c = 0; c < 8; ++c) sum += a[r * 8 + c] * x[c];
        out[r] = sum;
    }
    return out;
}

Matrix8 mul(const Matrix8 &a, const Matrix8 &b) {
    Matrix8 out{};
    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 8; ++c) {
            float sum = 0.0F;
            for (int k = 0; k < 8; ++k) sum += a[r * 8 + k] * b[k * 8 + c];
            out[r * 8 + c] = sum;
        }
    }
    return out;
}

Matrix8 transpose8(const Matrix8 &in) {
    Matrix8 out{};
    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 8; ++c) {
            out[c * 8 + r] = in[r * 8 + c];
        }
    }
    return out;
}

Vector4 project_mean(const Vector8 &mean) {
    return {mean[0], mean[1], mean[2], mean[3]};
}

Matrix4 project_covariance(const Matrix8 &covariance) {
    Matrix4 out{};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) out[r * 4 + c] = covariance[r * 8 + c];
    }
    return out;
}

bool invert4(const Matrix4 &in, Matrix4 &out) {
    Matrix4 aug = in;
    out = {};
    for (int i = 0; i < 4; ++i) out[i * 4 + i] = 1.0F;

    for (int i = 0; i < 4; ++i) {
        int pivot = i;
        float best = std::fabs(aug[i * 4 + i]);
        for (int r = i + 1; r < 4; ++r) {
            const float candidate = std::fabs(aug[r * 4 + i]);
            if (candidate > best) {
                best = candidate;
                pivot = r;
            }
        }
        if (best <= 1e-8F) return false;
        if (pivot != i) {
            for (int c = 0; c < 4; ++c) {
                std::swap(aug[i * 4 + c], aug[pivot * 4 + c]);
                std::swap(out[i * 4 + c], out[pivot * 4 + c]);
            }
        }

        const float scale = aug[i * 4 + i];
        for (int c = 0; c < 4; ++c) {
            aug[i * 4 + c] /= scale;
            out[i * 4 + c] /= scale;
        }
        for (int r = 0; r < 4; ++r) {
            if (r == i) continue;
            const float factor = aug[r * 4 + i];
            for (int c = 0; c < 4; ++c) {
                aug[r * 4 + c] -= factor * aug[i * 4 + c];
                out[r * 4 + c] -= factor * out[i * 4 + c];
            }
        }
    }
    return true;
}

Matrix8 motion_matrix() {
    Matrix8 out = identity8();
    for (int i = 0; i < 4; ++i) out[i * 8 + 4 + i] = 1.0F;
    return out;
}

Vector8 initiate_std(const std::array<float, 4> &measurement) {
    const float w = clamp_extent(measurement[2]);
    const float h = clamp_extent(measurement[3]);
    return {
        2.0F * kStdWeightPosition * w,
        2.0F * kStdWeightPosition * h,
        2.0F * kStdWeightPosition * w,
        2.0F * kStdWeightPosition * h,
        10.0F * kStdWeightVelocity * w,
        10.0F * kStdWeightVelocity * h,
        10.0F * kStdWeightVelocity * w,
        10.0F * kStdWeightVelocity * h,
    };
}

Vector8 predict_std(const Vector8 &mean) {
    const float w = clamp_extent(mean[2]);
    const float h = clamp_extent(mean[3]);
    return {
        kStdWeightPosition * w,
        kStdWeightPosition * h,
        kStdWeightPosition * w,
        kStdWeightPosition * h,
        kStdWeightVelocity * w,
        kStdWeightVelocity * h,
        kStdWeightVelocity * w,
        kStdWeightVelocity * h,
    };
}

Vector4 project_std(const Vector8 &mean) {
    const float w = clamp_extent(mean[2]);
    const float h = clamp_extent(mean[3]);
    return {
        kStdWeightPosition * w,
        kStdWeightPosition * h,
        kStdWeightPosition * w,
        kStdWeightPosition * h,
    };
}

}  // namespace

KalmanState
BoTSortKalman::initiate(float cx, float cy, float w, float h) const {
    KalmanState state{};
    state.mean[0] = cx;
    state.mean[1] = cy;
    state.mean[2] = w;
    state.mean[3] = h;

    const auto std = initiate_std({cx, cy, w, h});
    Vector8 var{};
    for (int i = 0; i < 8; ++i) var[i] = std[i] * std[i];
    state.covariance = diag8(var);
    return state;
}

KalmanState
BoTSortKalman::predict(const KalmanState &state) const {
    const auto motion = motion_matrix();
    const auto motion_t = transpose8(motion);

    KalmanState out{};
    out.mean = mul(motion, state.mean);

    const auto std = predict_std(state.mean);
    Vector8 var{};
    for (int i = 0; i < 8; ++i) var[i] = std[i] * std[i];
    const Matrix8 motion_cov = diag8(var);

    out.covariance = mul(mul(motion, state.covariance), motion_t);
    for (int i = 0; i < 64; ++i) out.covariance[i] += motion_cov[i];
    return out;
}

KalmanState
BoTSortKalman::update(const KalmanState &state, float cx, float cy, float w, float h) const {
    KalmanState out = state;

    const Vector4 projected_mean = project_mean(state.mean);
    Matrix4 projected_cov = project_covariance(state.covariance);
    const auto std = project_std(state.mean);
    Vector4 var{};
    for (int i = 0; i < 4; ++i) var[i] = std[i] * std[i];
    const Matrix4 innovation_cov = diag4(var);
    for (int i = 0; i < 16; ++i) projected_cov[i] += innovation_cov[i];

    Matrix4 projected_cov_inv{};
    if (!invert4(projected_cov, projected_cov_inv)) {
        out.mean[0] = cx;
        out.mean[1] = cy;
        out.mean[2] = w;
        out.mean[3] = h;
        return out;
    }

    std::array<float, 32> kalman_gain{};
    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 4; ++c) {
            float sum = 0.0F;
            for (int k = 0; k < 4; ++k) sum += state.covariance[r * 8 + k] * projected_cov_inv[k * 4 + c];
            kalman_gain[r * 4 + c] = sum;
        }
    }

    const Vector4 innovation = {
        cx - projected_mean[0],
        cy - projected_mean[1],
        w - projected_mean[2],
        h - projected_mean[3],
    };
    for (int r = 0; r < 8; ++r) {
        float delta = 0.0F;
        for (int c = 0; c < 4; ++c) delta += kalman_gain[r * 4 + c] * innovation[c];
        out.mean[r] += delta;
    }

    Matrix8 reduction{};
    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 8; ++c) {
            float sum = 0.0F;
            for (int k = 0; k < 4; ++k) sum += kalman_gain[r * 4 + k] * state.covariance[k * 8 + c];
            reduction[r * 8 + c] = sum;
        }
    }
    for (int i = 0; i < 64; ++i) out.covariance[i] = state.covariance[i] - reduction[i];
    return out;
}


}  // namespace botsort
