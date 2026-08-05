#include "botsort_matching.hpp"
#include "botsort_lapjv.hpp"

#include <algorithm>
#include <limits>
#include <vector>

namespace botsort {
namespace {

std::vector<int> hungarian_minimize(const std::vector<std::vector<double>> &cost) {
    const int n = static_cast<int>(cost.size());
    const double inf = std::numeric_limits<double>::infinity();

    std::vector<double> u(n + 1, 0.0);
    std::vector<double> v(n + 1, 0.0);
    std::vector<int> p(n + 1, 0);
    std::vector<int> way(n + 1, 0);

    for (int i = 1; i <= n; ++i) {
        p[0] = i;
        int j0 = 0;
        std::vector<double> minv(n + 1, inf);
        std::vector<bool> used(n + 1, false);
        do {
            used[j0] = true;
            const int i0 = p[j0];
            double delta = inf;
            int j1 = 0;
            for (int j = 1; j <= n; ++j) {
                if (used[j]) continue;
                const double cur = cost[i0 - 1][j - 1] - u[i0] - v[j];
                if (cur < minv[j]) {
                    minv[j] = cur;
                    way[j] = j0;
                }
                if (minv[j] < delta) {
                    delta = minv[j];
                    j1 = j;
                }
            }
            for (int j = 0; j <= n; ++j) {
                if (used[j]) {
                    u[p[j]] += delta;
                    v[j] -= delta;
                } else {
                    minv[j] -= delta;
                }
            }
            j0 = j1;
        } while (p[j0] != 0);

        do {
            const int j1 = way[j0];
            p[j0] = p[j1];
            j0 = j1;
        } while (j0 != 0);
    }

    std::vector<int> assignment(n, -1);
    for (int j = 1; j <= n; ++j) {
        if (p[j] > 0) assignment[p[j] - 1] = j - 1;
    }
    return assignment;
}

}  // namespace

float
iou_cost(const Detection &a, const Detection &b) {
    const float ix1 = std::max(a.x1, b.x1);
    const float iy1 = std::max(a.y1, b.y1);
    const float ix2 = std::min(a.x2, b.x2);
    const float iy2 = std::min(a.y2, b.y2);
    const float iw = std::max(0.0F, ix2 - ix1);
    const float ih = std::max(0.0F, iy2 - iy1);
    const float inter = iw * ih;
    const float area_a = std::max(0.0F, a.x2 - a.x1) * std::max(0.0F, a.y2 - a.y1);
    const float area_b = std::max(0.0F, b.x2 - b.x1) * std::max(0.0F, b.y2 - b.y1);
    const float denom = area_a + area_b - inter;
    if (denom <= 0.0F) return 1.0F;
    return 1.0F - (inter / denom);
}

float
fuse_score(float cost, float det_score) {
    const float sim = (1.0F - cost) * det_score;
    return 1.0F - sim;
}

bool
classes_compatible(int track_class_id, int det_class_id) {
    if (track_class_id < 0 || det_class_id < 0) return true;
    return track_class_id == det_class_id;
}

AssignmentResult
linear_assignment(const std::vector<std::vector<float>> &cost_matrix, float thresh) {
    AssignmentResult result{};
    const int rows = static_cast<int>(cost_matrix.size());
    const int cols = rows > 0 ? static_cast<int>(cost_matrix[0].size()) : 0;
    if (rows == 0 || cols == 0) {
        for (int r = 0; r < rows; ++r) result.unmatched_rows.push_back(r);
        for (int c = 0; c < cols; ++c) result.unmatched_cols.push_back(c);
        return result;
    }

    const int size = rows + cols;
    std::vector<std::vector<double>> expanded(size, std::vector<double>(size, static_cast<double>(thresh) / 2.0));
    for (int r = rows; r < size; ++r) {
        for (int c = cols; c < size; ++c) expanded[r][c] = 0.0;
    }
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) expanded[r][c] = cost_matrix[r][c];
    }

    std::vector<int> assignment(size, -1);
    std::vector<int> column_assignment(size, -1);
    std::vector<double *> row_ptrs(size, nullptr);
    for (int r = 0; r < size; ++r) row_ptrs[r] = expanded[r].data();
    const int lap_status = lapjv_internal(
        static_cast<uint_t>(size), row_ptrs.data(), assignment.data(), column_assignment.data());
    if (lap_status != 0) {
        assignment = hungarian_minimize(expanded);
    }
    std::vector<bool> matched_cols(cols, false);

    for (int r = 0; r < rows; ++r) {
        const int assigned = assignment[r];
        if (assigned >= 0 && assigned < cols && cost_matrix[r][assigned] <= thresh) {
            result.matches.emplace_back(r, assigned);
            matched_cols[assigned] = true;
        } else {
            result.unmatched_rows.push_back(r);
        }
    }
    for (int c = 0; c < cols; ++c) {
        if (!matched_cols[c]) result.unmatched_cols.push_back(c);
    }
    return result;
}

FrameDetections
restore_input_order(const FrameDetections &detections, std::size_t input_size) {
    FrameDetections ordered(input_size);
    for (const auto &det : detections) {
        if (det.input_index < 0) continue;
        const std::size_t index = static_cast<std::size_t>(det.input_index);
        if (index >= ordered.size()) continue;
        ordered[index] = det;
    }
    return ordered;
}

}  // namespace botsort
