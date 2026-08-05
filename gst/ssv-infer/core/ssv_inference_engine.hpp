#pragma once

#include "core/ssv_backend_factory.hpp"
#include "core/ssv_inference_buffer.hpp"
#include "core/ssv_inference_stats.hpp"
#include "ssv_meta.hpp"
#include "ssv_model_contract.hpp"
#include "model/ssv_yolo_parser.hpp"

#include <memory>
#include <optional>
#include <stop_token>

namespace ssv::infer {

struct SsvInferenceRequest;

struct SsvInferenceRunResult {
    SsvDetectionFrame detections;
    SsvInferenceStageTimings timings;
};

class InferenceEngine {
public:
    explicit InferenceEngine(
        std::unique_ptr<InferenceBackend> backend = {},
        std::shared_ptr<SsvInferenceBufferAllocator> allocator = {});

    void start(const InferenceConfig &config);
    void stop();
    bool loaded() const { return backend_ != nullptr; }

    SsvInferenceRunResult run(
        const SsvInferenceRequest &request,
        std::stop_token stop_token);
    BackendInfo backend_info() const;
    const SsvModelContract &model_contract() const;

private:
    InferenceConfig config_;
    std::unique_ptr<InferenceBackend> backend_;
    std::shared_ptr<SsvInferenceBufferAllocator> allocator_;
    ModelMetadata metadata_;
    std::optional<SsvModelContract> model_contract_;
    YoloOutputParser parser_;
};

std::vector<std::string> load_label_map(const std::string &path);

} // namespace ssv::infer
