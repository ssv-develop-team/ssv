#include "core/ssv_inference_engine.hpp"

#include "ssv_inference_service.hpp"

#include <algorithm>
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
    input_contract_ = ssv_model_input_contract_validate(
        metadata_, config.model_family, config.output_format);
    model_contract_ = ssv_model_contract_validate(
        metadata_, config.model_family, config.output_format,
        config.preprocess->resize_mode);
    preprocessor_ = std::make_unique<SsvImagePreprocessor>(
        ssv_make_preprocess_plan(
            input_contract_.input, *config.preprocess));
    hardware_preprocess_enabled_ = false;
    bool hardware_preprocess_supported = false;
    if (config.preprocess->execution != SsvPreprocessExecution::Cpu) {
        try {
            hardware_preprocess_supported =
                backend_->supports_hardware_preprocess(preprocessor_->plan());
        } catch (const std::exception &error) {
            if (config.preprocess->execution == SsvPreprocessExecution::Cuda) {
                throw std::runtime_error(
                    std::string("inference.start: CUDA preprocess capability "
                                "query failed: ")
                    + error.what());
            }
        } catch (...) {
            if (config.preprocess->execution == SsvPreprocessExecution::Cuda) {
                throw std::runtime_error(
                    "inference.start: CUDA preprocess capability query failed");
            }
        }
    }
    if (config.preprocess->execution == SsvPreprocessExecution::Cuda
        && !hardware_preprocess_supported) {
        throw std::runtime_error(
            "inference.start: requested CUDA preprocessing is unavailable "
            "for the selected backend");
    }
    hardware_preprocess_enabled_ =
        config.preprocess->execution != SsvPreprocessExecution::Cpu
        && hardware_preprocess_supported;
    input_buffer_ = allocator_->allocate(
        input_contract_.byte_size, alignof(float));
    auto input_data = input_buffer_.as_span<float>();
    std::fill(input_data.begin(), input_data.end(), 0.0F);
    input_view_ = {&metadata_.inputs.front(), input_data};
    backend_->warmup(input_view_);
    metadata_.backend = backend_->info();
    parser_.configure(config, metadata_, std::move(labels));
}

void InferenceEngine::stop()
{
    backend_.reset();
    metadata_ = {};
    model_contract_.reset();
    input_contract_ = {};
    input_view_ = {};
    input_buffer_ = {};
    preprocessor_.reset();
    hardware_preprocess_enabled_ = false;
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

    if (!preprocessor_ || input_view_.spec == nullptr)
        throw std::logic_error("inference input plan is not initialized");
    SsvBackendRunResult backend_result;
    if (hardware_preprocess_enabled_) {
        const SsvHardwarePreprocessInput hardware_input {
            rgba,
            preprocessor_->plan(),
            input_view_,
        };
        backend_result = backend_->infer_hardware(
            hardware_input, stop_token);
        result.timings.normalize_layout_us =
            backend_result.timings.preprocess_us;
    } else {
        const auto preprocess_timing = preprocessor_->run(
            rgba, input_buffer_.as_span<float>());
        result.timings.normalize_layout_us =
            preprocess_timing.normalize_layout_us;
        backend_result = backend_->infer(input_view_, stop_token);
    }
    result.timings.backend_h2d_us = backend_result.timings.h2d_us;
    result.timings.backend_execution_us =
        backend_result.timings.execution_us;
    result.timings.backend_d2h_us = backend_result.timings.d2h_us;
    result.timings.backend_unattributed_us =
        backend_result.timings.unattributed_us;
    const auto postprocess_start = Clock::now();
    result.detections.detections = parser_.parse(
        backend_result.outputs, transform);
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

std::string InferenceEngine::input_contract_description() const
{
    if (!backend_ || !model_contract_)
        throw std::logic_error("inference engine is not started");
    return input_contract_.description();
}

const SsvModelContract &InferenceEngine::model_contract() const
{
    if (!model_contract_)
        throw std::logic_error("inference engine is not started");
    return *model_contract_;
}

} // namespace ssv::infer
