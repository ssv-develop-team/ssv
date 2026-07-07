#pragma once

#include "ssv_tensor.hpp"

namespace ssv::infer {

struct PreprocessResult {
    Tensor input;
    int original_width = 0;
    int original_height = 0;
    int model_width = 0;
    int model_height = 0;
};

class Preprocessor {
public:
    PreprocessResult run(const SsvVideoFrame &frame, const TensorSpec &input_spec) const;
};

} // namespace ssv::infer
