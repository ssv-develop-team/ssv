#include "ssv_meta.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>

PreprocessTransform ssv_make_letterbox_transform(
    int source_width,
    int source_height,
    int model_width,
    int model_height)
{
    if (source_width <= 0 || source_height <= 0
        || model_width <= 0 || model_height <= 0) {
        throw std::invalid_argument(
            "letterbox dimensions must be positive");
    }

    const bool width_limited =
        static_cast<std::int64_t>(source_width) * model_height
        >= static_cast<std::int64_t>(source_height) * model_width;
    const float scale = width_limited
        ? static_cast<float>(model_width) / source_width
        : static_cast<float>(model_height) / source_height;
    const int active_width = width_limited
        ? model_width
        : std::clamp(
              static_cast<int>(std::lround(source_width * scale)),
              1,
              model_width);
    const int active_height = width_limited
        ? std::clamp(
              static_cast<int>(std::lround(source_height * scale)),
              1,
              model_height)
        : model_height;
    const int horizontal_padding = model_width - active_width;
    const int vertical_padding = model_height - active_height;
    const int pad_left = horizontal_padding / 2;
    const int pad_top = vertical_padding / 2;

    return {
        source_width,
        source_height,
        model_width,
        model_height,
        scale,
        pad_left,
        pad_top,
        horizontal_padding - pad_left,
        vertical_padding - pad_top,
    };
}

std::optional<SsvDetection> ssv_unmap_model_detection(
    SsvDetection detection,
    const PreprocessTransform &transform)
{
    if (transform.source_width <= 0 || transform.source_height <= 0
        || transform.model_width <= 0 || transform.model_height <= 0
        || !std::isfinite(transform.scale) || transform.scale <= 0.0F
        || transform.pad_left < 0 || transform.pad_top < 0
        || transform.pad_right < 0 || transform.pad_bottom < 0
        || transform.pad_left + transform.pad_right >= transform.model_width
        || transform.pad_top + transform.pad_bottom >= transform.model_height) {
        throw std::invalid_argument("invalid preprocess transform");
    }

    const float source_width = static_cast<float>(transform.source_width);
    const float source_height = static_cast<float>(transform.source_height);
    detection.x1 = (detection.x1 - transform.pad_left)
        / transform.scale / source_width;
    detection.y1 = (detection.y1 - transform.pad_top)
        / transform.scale / source_height;
    detection.x2 = (detection.x2 - transform.pad_left)
        / transform.scale / source_width;
    detection.y2 = (detection.y2 - transform.pad_top)
        / transform.scale / source_height;

    if (!std::isfinite(detection.x1) || !std::isfinite(detection.y1)
        || !std::isfinite(detection.x2) || !std::isfinite(detection.y2)) {
        return std::nullopt;
    }

    detection.x1 = std::clamp(detection.x1, 0.0F, 1.0F);
    detection.y1 = std::clamp(detection.y1, 0.0F, 1.0F);
    detection.x2 = std::clamp(detection.x2, 0.0F, 1.0F);
    detection.y2 = std::clamp(detection.y2, 0.0F, 1.0F);
    if (detection.x2 <= detection.x1 || detection.y2 <= detection.y1)
        return std::nullopt;
    return detection;
}
