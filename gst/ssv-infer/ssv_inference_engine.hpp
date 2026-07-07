#pragma once

#include "ssv_backend_factory.hpp"
#include "ssv_meta.hpp"
#include "ssv_preprocessor.hpp"
#include "ssv_yolo_parser.hpp"

#include <memory>

namespace ssv::infer {

class InferenceEngine {
public:
    void start(const InferenceConfig &config);
    void stop();
    bool loaded() const { return backend_ != nullptr; }

    SsvFrameDetections run(const SsvVideoFrame &frame);
    BackendInfo backend_info() const;

private:
    InferenceConfig config_;
    std::unique_ptr<InferenceBackend> backend_;
    ModelMetadata metadata_;
    Preprocessor preprocessor_;
    YoloOutputParser parser_;
};

std::vector<std::string> load_label_map(const std::string &path);
std::vector<std::string> default_coco_labels();

} // namespace ssv::infer
