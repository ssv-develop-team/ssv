#pragma once

#include "botsort_types.hpp"

namespace botsort {

inline Detection to_pixel_detection(const Detection &source, int width, int height) {
    Detection result = source;
    if (width <= 0 || height <= 0) return result;
    result.x1 *= static_cast<float>(width);
    result.x2 *= static_cast<float>(width);
    result.y1 *= static_cast<float>(height);
    result.y2 *= static_cast<float>(height);
    return result;
}

inline Detection to_normalized_detection(const Detection &source, int width, int height) {
    Detection result = source;
    if (width <= 0 || height <= 0) return result;
    result.x1 /= static_cast<float>(width);
    result.x2 /= static_cast<float>(width);
    result.y1 /= static_cast<float>(height);
    result.y2 /= static_cast<float>(height);
    return result;
}

}  // namespace botsort
