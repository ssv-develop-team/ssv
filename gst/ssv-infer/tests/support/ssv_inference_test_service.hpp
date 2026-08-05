#pragma once

#include "ssv_inference_service.hpp"

#include <functional>
#include <vector>

namespace ssv::infer {

struct SsvInferenceTestDetection {
    float x1 = 0.1F;
    float y1 = 0.2F;
    float x2 = 0.4F;
    float y2 = 0.8F;
    float confidence = 0.95F;
    int class_id = 0;
};

struct SsvInferenceTestServiceOptions {
    // Each entry supplies one normalized model-space detection for one
    // inference call. An empty sequence uses the default value above; after
    // exhaustion, the final entry is reused.
    std::vector<SsvInferenceTestDetection> detection_sequence;
    // If set, called synchronously exactly once when the service releases its
    // backend. Exceptions are swallowed so observation cannot alter cleanup.
    std::function<void()> on_backend_destroyed;
};

[[nodiscard]] SsvInferenceServicePtr ssv_inference_test_service_create(
    const ssv::SsvInferenceConfig &config,
    SsvInferenceTestServiceOptions options = {});

} // namespace ssv::infer
