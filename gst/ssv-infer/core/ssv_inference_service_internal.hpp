#pragma once

#include "core/ssv_inference_backend.hpp"
#include "ssv_inference_service.hpp"

#include <memory>

namespace ssv::infer {

[[nodiscard]] SsvInferenceServicePtr
ssv_inference_service_create_with_backend(
    const ssv::SsvInferenceConfig &config,
    std::unique_ptr<InferenceBackend> backend,
    std::shared_ptr<SsvInferenceBufferAllocator> allocator = {});

} // namespace ssv::infer
