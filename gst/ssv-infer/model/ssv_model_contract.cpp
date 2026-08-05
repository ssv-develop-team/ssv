#include "model/ssv_model_contract_internal.hpp"

#include <charconv>
#include <cctype>
#include <limits>
#include <string_view>

namespace ssv::infer {
namespace {

constexpr std::string_view WRAPPER_CONTRACT = "rgba_u8_nhwc_v1";

const std::string &required_property(
    const ModelMetadata &metadata,
    std::string_view key)
{
    const auto found = metadata.properties.find(std::string(key));
    if (found == metadata.properties.end() || found->second.empty()) {
        throw SsvModelContractError(
            "wrapper metadata is missing " + std::string(key));
    }
    return found->second;
}

void require_property(
    const ModelMetadata &metadata,
    std::string_view key,
    std::string_view expected)
{
    const auto &actual = required_property(metadata, key);
    if (actual != expected) {
        throw SsvModelContractError(
            "wrapper metadata " + std::string(key) + " must be '"
            + std::string(expected) + "'");
    }
}

int positive_integer_property(
    const ModelMetadata &metadata,
    std::string_view key)
{
    const auto &text = required_property(metadata, key);
    int value = 0;
    const auto parsed = std::from_chars(
        text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size()
        || value <= 0) {
        throw SsvModelContractError(
            "wrapper metadata " + std::string(key)
            + " must be a positive integer");
    }
    return value;
}

std::string_view model_family_name(ModelFamily family)
{
    if (family == ModelFamily::Yolo)
        return "yolo";
    throw SsvModelContractError("model family must be explicitly resolved");
}

std::string_view output_format_name(OutputFormat format)
{
    switch (format) {
    case OutputFormat::YoloV5: return "yolov5";
    case OutputFormat::YoloV8: return "yolov8";
    case OutputFormat::YoloNx6: return "yolo_nx6";
    }
    throw SsvModelContractError("model output format must be explicitly resolved");
}

bool is_lower_hex_sha256(std::string_view value)
{
    if (value.size() != 64)
        return false;
    for (const unsigned char character : value) {
        if (!std::isdigit(character)
            && (character < static_cast<unsigned char>('a')
                || character > static_cast<unsigned char>('f'))) {
            return false;
        }
    }
    return true;
}

} // namespace

SsvModelContract ssv_model_contract_validate(
    const ModelMetadata &metadata,
    ModelFamily model_family,
    OutputFormat output_format)
{
    require_property(metadata, "ssv.wrapper.contract", WRAPPER_CONTRACT);
    require_property(metadata, "ssv.wrapper.dtype", "uint8");
    require_property(metadata, "ssv.wrapper.layout", "NHWC");
    require_property(
        metadata, "ssv.wrapper.channel_rule", "drop_alpha_keep_rgb");
    require_property(
        metadata, "ssv.wrapper.normalization", "divide_by_255");
    require_property(metadata, "ssv.wrapper.tool", "ssv.prepare_wrapper");
    static_cast<void>(required_property(
        metadata, "ssv.wrapper.tool_version"));
    require_property(
        metadata, "ssv.wrapper.model_family", model_family_name(model_family));
    require_property(
        metadata, "ssv.wrapper.output_format", output_format_name(output_format));

    const auto &source_sha256 =
        required_property(metadata, "ssv.wrapper.source_sha256");
    if (!is_lower_hex_sha256(source_sha256)) {
        throw SsvModelContractError(
            "wrapper metadata ssv.wrapper.source_sha256 must be lowercase SHA-256");
    }

    if (metadata.inputs.size() != 1)
        throw SsvModelContractError("wrapper must have exactly one input");
    if (metadata.outputs.empty())
        throw SsvModelContractError("wrapper must have at least one output");

    const TensorSpec &input = metadata.inputs.front();
    if (input.dtype != DataType::Uint8)
        throw SsvModelContractError("wrapper input must use uint8");
    if (input.layout != TensorLayout::Nhwc)
        throw SsvModelContractError("wrapper input must use NHWC layout");
    if (input.shape.size() != 4 || input.shape[0] != 1
        || input.shape[3] != 4 || input.shape[1] <= 0
        || input.shape[2] <= 0) {
        throw SsvModelContractError(
            "wrapper input must be static uint8 [1,H,W,4]");
    }

    const int height = positive_integer_property(
        metadata, "ssv.wrapper.height");
    const int width = positive_integer_property(
        metadata, "ssv.wrapper.width");
    if (input.shape[1] != height || input.shape[2] != width) {
        throw SsvModelContractError(
            "wrapper input shape does not match width/height metadata");
    }
    if (static_cast<std::size_t>(width)
        > std::numeric_limits<std::size_t>::max()
            / static_cast<std::size_t>(height) / 4U) {
        throw SsvModelContractError("wrapper input byte size overflows size_t");
    }

    return {
        width,
        height,
        static_cast<std::size_t>(width)
            * static_cast<std::size_t>(height) * 4U,
        std::string(WRAPPER_CONTRACT),
        source_sha256,
    };
}

} // namespace ssv::infer
