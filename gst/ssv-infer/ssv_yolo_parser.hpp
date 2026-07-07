#pragma once

#include "ssv_inference_config.hpp"
#include "ssv_meta.hpp"
#include "ssv_preprocessor.hpp"

#include <string>
#include <vector>

namespace ssv::infer {

class YoloOutputParser {
public:
    void configure(const InferenceConfig &config,
                   const ModelMetadata &metadata,
                   std::vector<std::string> labels);

    std::vector<SsvDetection> parse(const std::vector<Tensor> &outputs,
                                    const PreprocessResult &preprocess) const;

    OutputFormat output_format() const { return output_format_; }
    int num_classes() const { return num_classes_; }

private:
    OutputFormat output_format_ = OutputFormat::Auto;
    float confidence_threshold_ = 0.5f;
    std::vector<std::string> labels_;
    int target_class_id_ = -1;
    int num_classes_ = 0;
};

} // namespace ssv::infer
