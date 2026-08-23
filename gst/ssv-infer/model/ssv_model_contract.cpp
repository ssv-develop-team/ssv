#include "model/ssv_model_contract_internal.hpp"

#include <limits>
#include <string>

namespace ssv::infer {
namespace {

std::size_t tensor_element_count(
    const TensorSpec &spec,
    const char *kind)
{
    if (spec.shape.empty())
        throw SsvModelContractError(
            std::string(kind) + " tensor shape must not be empty");
    std::size_t count = 1;
    for (const auto dimension : spec.shape) {
        if (dimension <= 0) {
            throw SsvModelContractError(
                std::string(kind)
                + " tensor must have static positive dimensions");
        }
        const auto value = static_cast<std::size_t>(dimension);
        if (count > std::numeric_limits<std::size_t>::max() / value) {
            throw SsvModelContractError(
                std::string(kind) + " tensor element count overflows size_t");
        }
        count *= value;
    }
    return count;
}

void validate_model_family_and_output_format(
    ModelFamily model_family,
    OutputFormat output_format)
{
    if (model_family != ModelFamily::Yolo)
        throw SsvModelContractError("model family must be explicitly resolved");
    switch (output_format) {
    case OutputFormat::YoloV5:
    case OutputFormat::YoloV8:
    case OutputFormat::YoloNx6:
        return;
    }
    throw SsvModelContractError("model output format must be explicitly resolved");
}

} // namespace

std::string SsvModelInputContract::description() const
{
    std::string result = "float32[";
    for (std::size_t index = 0; index < input.shape.size(); ++index) {
        if (index != 0)
            result += ',';
        result += std::to_string(input.shape[index]);
    }
    result += "]:NCHW";
    return result;
}

SsvModelInputContract ssv_model_input_contract_validate(
    const ModelMetadata &metadata,
    ModelFamily model_family,
    OutputFormat output_format)
{
    validate_model_family_and_output_format(model_family, output_format);
    if (metadata.inputs.size() != 1)
        throw SsvModelContractError(
            "raw model must have exactly one input tensor");
    if (metadata.outputs.empty())
        throw SsvModelContractError(
            "raw model must have at least one output tensor");

    const TensorSpec &input = metadata.inputs.front();
    if (input.dtype != DataType::Float32)
        throw SsvModelContractError("raw model input must use float32");
    if (input.layout != TensorLayout::Nchw)
        throw SsvModelContractError("raw model input must use NCHW layout");
    if (input.shape.size() != 4 || input.shape[0] != 1
        || input.shape[1] != 3 || input.shape[2] <= 0
        || input.shape[3] <= 0) {
        throw SsvModelContractError(
            "raw model input must be static float32 [1,3,H,W]");
    }

    const auto input_elements = tensor_element_count(input, "raw model input");
    if (input_elements > std::numeric_limits<std::size_t>::max() / sizeof(float)) {
        throw SsvModelContractError(
            "raw model input byte size overflows size_t");
    }
    for (const auto &output : metadata.outputs) {
        if (output.dtype != DataType::Float32)
            throw SsvModelContractError("raw model outputs must use float32");
        static_cast<void>(tensor_element_count(output, "raw model output"));
    }

    return {
        input,
        input_elements,
        input_elements * sizeof(float),
    };
}

SsvModelContract ssv_model_contract_validate(
    const ModelMetadata &metadata,
    ModelFamily model_family,
    OutputFormat output_format,
    ssv::SsvResizeMode resize_mode)
{
    const auto input = ssv_model_input_contract_validate(
        metadata, model_family, output_format);
    if (input.input.shape[2] > std::numeric_limits<int>::max()
        || input.input.shape[3] > std::numeric_limits<int>::max()) {
        throw SsvModelContractError(
            "raw model input dimensions exceed runtime geometry limits");
    }
    return {
        static_cast<int>(input.input.shape[3]),
        static_cast<int>(input.input.shape[2]),
        resize_mode,
    };
}

} // namespace ssv::infer
