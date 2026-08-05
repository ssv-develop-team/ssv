#pragma once

#include "core/ssv_inference_backend.hpp"

namespace ssv::infer {

std::unique_ptr<InferenceBackend> create_backend(const InferenceConfig &config);

} // namespace ssv::infer
