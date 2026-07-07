#include "ssv_inference_engine.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace ssv::infer {

namespace {

static constexpr const char *COCO_NAMES[] = {
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train",
    "truck", "boat", "traffic light", "fire hydrant", "stop sign",
    "parking meter", "bench", "bird", "cat", "dog", "horse", "sheep",
    "cow", "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella",
    "handbag", "tie", "suitcase", "frisbee", "skis", "snowboard",
    "sports ball", "kite", "baseball bat", "baseball glove", "skateboard",
    "surfboard", "tennis racket", "bottle", "wine glass", "cup", "fork",
    "knife", "spoon", "bowl", "banana", "apple", "sandwich", "orange",
    "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair",
    "couch", "potted plant", "bed", "dining table", "toilet", "tv",
    "laptop", "mouse", "remote", "keyboard", "cell phone", "microwave",
    "oven", "toaster", "sink", "refrigerator", "book", "clock", "vase",
    "scissors", "teddy bear", "hair drier", "toothbrush"
};

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

std::vector<std::string> default_coco_labels()
{
    return std::vector<std::string>(COCO_NAMES, COCO_NAMES + std::size(COCO_NAMES));
}

std::vector<std::string> load_label_map(const std::string &path)
{
    if (path.empty())
        return default_coco_labels();

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

void InferenceEngine::start(const InferenceConfig &config)
{
    validate_inference_config(config);
    config_ = config;
    std::vector<std::string> labels = load_label_map(config.label_map);
    backend_ = create_backend(config);
    metadata_ = backend_->load(config);
    if (metadata_.inputs.size() != 1)
        throw std::runtime_error("only single-input image models are supported");
    parser_.configure(config, metadata_, std::move(labels));
}

void InferenceEngine::stop()
{
    backend_.reset();
    metadata_ = {};
}

SsvFrameDetections InferenceEngine::run(const SsvVideoFrame &frame)
{
    SsvFrameDetections detections;
    detections.frame_id = frame.frame_id;
    std::snprintf(detections.source_id, sizeof(detections.source_id), "%s", frame.source_id.c_str());

    if (!backend_)
        return detections;

    PreprocessResult preprocessed = preprocessor_.run(frame, metadata_.inputs[0]);
    std::vector<Tensor> outputs = backend_->infer(std::span<const Tensor>(&preprocessed.input, 1));
    detections.detections = parser_.parse(outputs, preprocessed);
    return detections;
}

BackendInfo InferenceEngine::backend_info() const
{
    if (!backend_)
        return {};
    return backend_->info();
}

} // namespace ssv::infer
