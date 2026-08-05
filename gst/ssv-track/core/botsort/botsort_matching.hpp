#pragma once

#include "botsort_types.hpp"

#include <utility>
#include <vector>

namespace botsort {

struct AssignmentResult {
    std::vector<std::pair<int, int>> matches;
    std::vector<int> unmatched_rows;
    std::vector<int> unmatched_cols;
};

float iou_cost(const Detection &a, const Detection &b);
float fuse_score(float cost, float det_score);
bool classes_compatible(int track_class_id, int det_class_id);
AssignmentResult linear_assignment(const std::vector<std::vector<float>> &cost_matrix, float thresh);
FrameDetections restore_input_order(const FrameDetections &detections, std::size_t input_size);

}  // namespace botsort
