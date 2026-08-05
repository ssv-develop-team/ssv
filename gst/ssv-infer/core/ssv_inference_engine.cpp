#include "core/ssv_inference_engine.hpp"

#include "model/ssv_model_contract_internal.hpp"
#include "ssv_inference_service.hpp"

#include <cstdio>
#include <chrono>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace ssv::infer {

namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t elapsed_us(Clock::time_point started)
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now() - started)
            .count());
}

std::string trim_label_line(const std::string &line)
{
    const char *spaces = " \t\r\n";
    size_t begin = line.find_first_not_of(spaces);
    if (begin == std::string::npos)
        return "";
    size_t end = line.find_last_not_of(spaces);
    return line.substr(begin, end - begin + 1);
}

} // namespace

std::vector<std::string> load_label_map(const std::string &path)
{
    if (path.empty())
        throw std::invalid_argument(
            "inference.model.label_map must not be empty");

    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("label-map not found: " + path);

    std::vector<std::string> labels;
    std::string line;
    while (std::getline(input, line)) {
        std::string value = trim_label_line(line);
        if (value.empty() || value[0] == '#')
            continue;
        labels.push_back(value);
    }

    if (labels.empty())
        throw std::runtime_error("label-map has no labels: " + path);
    return labels;
}

InferenceEngine::InferenceEngine(
    std::unique_ptr<InferenceBackend> backend,
    std::shared_ptr<SsvInferenceBufferAllocator> allocator)
    : backend_(std::move(backend))
    , allocator_(std::move(allocator))
{
    if (!allocator_)
        allocator_ = std::make_shared<SsvDefaultInferenceBufferAllocator>();
}

void InferenceEngine::start(const InferenceConfig &config)
{
    validate_inference_config(config);
    config_ = config;
    std::vector<std::string> labels = load_label_map(config.label_map);
    if (!backend_)
        backend_ = create_backend(config);
    metadata_ = backend_->load(config, *allocator_);
    model_contract_ = ssv_model_contract_validate(
        metadata_, config.model_family, config.output_format);
    backend_->warmup();
    metadata_.backend = backend_->info();
    parser_.configure(config, metadata_, std::move(labels));
}

void InferenceEngine::stop()
{
    backend_.reset();
    metadata_ = {};
    model_contract_.reset();
}

SsvInferenceRunResult InferenceEngine::run(
    const SsvInferenceRequest &request,
    std::stop_token stop_token)
{
    const auto total_start = Clock::now();
    SsvInferenceRunResult result;
    result.detections.frame_id = request.frame_id;
    result.detections.source_id = request.source_id;
    result.detections.analysis_frame = request.analysis_frame;

    if (!backend_ || !model_contract_)
        throw std::logic_error("inference engine is not started");
    if (request.source_id.empty())
        throw std::invalid_argument("inference source_id must not be empty");
    if (!request.analysis_frame)
        throw std::invalid_argument("inference analysis frame must not be null");

    const auto &analysis_frame = *request.analysis_frame;
    const auto &rgba = analysis_frame.view();
    const auto &transform = analysis_frame.transform();
    result.detections.timing = analysis_frame.timing();

    const auto &contract = *model_contract_;
    const std::size_t row_bytes =
        static_cast<std::size_t>(contract.width) * 4U;
    if (rgba.width != contract.width
        || rgba.height != contract.height) {
        throw std::invalid_argument(
            "RGBA frame dimensions do not match wrapper input contract");
    }
    if (rgba.stride != row_bytes) {
        throw std::invalid_argument(
            "RGBA frame must be tightly packed before inference");
    }
    if (rgba.bytes.size() < contract.input_bytes) {
        throw std::invalid_argument(
            "RGBA frame is smaller than wrapper input contract");
    }

    const SsvUint8TensorView input {
        &metadata_.inputs.front(),
        rgba.bytes.first(contract.input_bytes),
    };
    const auto device_start = Clock::now();
    const auto outputs = backend_->infer(input, stop_token);
    result.timings.device_us = elapsed_us(device_start);
    // Backends expose reusable host output views. Any Provider-controlled
    // device-to-host synchronization is therefore included in device_us;
    // SSV performs no additional output copy after infer() returns.
    result.timings.output_copy_us = 0;
    const auto postprocess_start = Clock::now();
    result.detections.detections = parser_.parse(outputs, transform);
    result.timings.postprocess_us = elapsed_us(postprocess_start);
    result.timings.total_us = elapsed_us(total_start);
    return result;
}

BackendInfo InferenceEngine::backend_info() const
{
    if (!backend_)
        return {};
    return backend_->info();
}

const SsvModelContract &InferenceEngine::model_contract() const
{
    if (!model_contract_)
        throw std::logic_error("inference engine is not started");
    return *model_contract_;
}

} // namespace ssv::infer
