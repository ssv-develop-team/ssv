#include "backends/tensorrt/ssv_tensorrt_manifest.hpp"

#include <glib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <string_view>

namespace ssv::infer {
namespace {

constexpr std::string_view MANIFEST_SCHEMA = "ssv.tensorrt-engine-manifest";
constexpr int MANIFEST_SCHEMA_VERSION = 1;

void require_object_keys(const nlohmann::json &value,
    std::string_view path,
    std::initializer_list<std::string_view> allowed)
{
    if (!value.is_object()) {
        throw SsvTensorRtManifestError(
            std::string(path) + " must be an object");
    }
    for (const auto &[key, ignored] : value.items()) {
        static_cast<void>(ignored);
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
            throw SsvTensorRtManifestError(
                std::string(path) + " unknown key: " + key);
        }
    }
}

const nlohmann::json &required_value(
    const nlohmann::json &object, std::string_view key, std::string_view path)
{
    const auto found = object.find(key);
    if (found == object.end()) {
        throw SsvTensorRtManifestError(
            std::string(path) + " is missing " + std::string(key));
    }
    return *found;
}

std::string required_string(
    const nlohmann::json &object, std::string_view key, std::string_view path)
{
    const auto &value = required_value(object, key, path);
    if (!value.is_string()) {
        throw SsvTensorRtManifestError(
            std::string(path) + "." + std::string(key) + " must be a string");
    }
    return value.get<std::string>();
}

int required_integer(
    const nlohmann::json &object, std::string_view key, std::string_view path)
{
    const auto &value = required_value(object, key, path);
    if (!value.is_number_integer()) {
        throw SsvTensorRtManifestError(
            std::string(path) + "." + std::string(key) + " must be an integer");
    }
    try {
        return value.get<int>();
    } catch (const std::exception &) {
        throw SsvTensorRtManifestError(
            std::string(path) + "." + std::string(key)
            + " is outside the supported integer range");
    }
}

std::string sha256(std::span<const std::byte> bytes)
{
    gchar *digest = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
        reinterpret_cast<const guchar *>(bytes.data()),
        bytes.size());
    if (digest == nullptr)
        throw SsvTensorRtManifestError("failed to calculate engine SHA-256");
    std::string result(digest);
    g_free(digest);
    return result;
}

bool lower_hex_sha256(std::string_view value)
{
    return value.size() == 64
           && std::all_of(
               value.begin(), value.end(), [](unsigned char character) {
                   return std::isdigit(character) != 0
                          || (character >= 'a' && character <= 'f');
               });
}

ssv::SsvPrecision parse_precision(const std::string &value)
{
    if (value == "fp32")
        return ssv::SsvPrecision::Fp32;
    if (value == "fp16")
        return ssv::SsvPrecision::Fp16;
    throw SsvTensorRtManifestError(
        "TensorRT engine manifest precision must be fp32 or fp16");
}

TensorSpec parse_input(const nlohmann::json &input)
{
    require_object_keys(input,
        "TensorRT engine manifest wrapper.input",
        {"name", "dtype", "layout", "shape"});
    TensorSpec spec;
    constexpr std::string_view path = "TensorRT engine manifest wrapper.input";
    spec.name = required_string(input, "name", path);
    if (spec.name.empty()) {
        throw SsvTensorRtManifestError(
            "TensorRT engine manifest input name must not be empty");
    }
    if (required_string(input, "dtype", path) != "uint8") {
        throw SsvTensorRtManifestError(
            "TensorRT engine manifest input dtype must be uint8");
    }
    if (required_string(input, "layout", path) != "NHWC") {
        throw SsvTensorRtManifestError(
            "TensorRT engine manifest input layout must be NHWC");
    }
    spec.dtype = DataType::Uint8;
    spec.layout = TensorLayout::Nhwc;
    const auto &shape = required_value(input, "shape", path);
    if (!shape.is_array()) {
        throw SsvTensorRtManifestError(
            "TensorRT engine manifest wrapper.input.shape must be an array");
    }
    for (const auto &dimension : shape) {
        if (!dimension.is_number_integer()) {
            throw SsvTensorRtManifestError(
                "TensorRT engine manifest wrapper.input.shape dimensions must "
                "be "
                "integers");
        }
        spec.shape.push_back(dimension.get<std::int64_t>());
    }
    if (spec.shape.size() != 4 || spec.shape[0] != 1 || spec.shape[1] <= 0
        || spec.shape[2] <= 0 || spec.shape[3] != 4) {
        throw SsvTensorRtManifestError(
            "TensorRT engine manifest input must be static uint8 [1,H,W,4]");
    }
    return spec;
}

void require_hash(std::string_view name, const std::string &value)
{
    if (!lower_hex_sha256(value)) {
        throw SsvTensorRtManifestError(
            std::string(name) + " must be lowercase SHA-256");
    }
}

void require_runtime_match(const SsvTensorRtEngineManifest &manifest,
    const SsvTensorRtRuntimeDescriptor &runtime)
{
    if (manifest.tensorrt_version != runtime.tensorrt_version) {
        throw SsvTensorRtManifestError(
            "TensorRT engine manifest TensorRT version does not match runtime");
    }
    if (manifest.cuda_runtime_version != runtime.cuda_runtime_version) {
        throw SsvTensorRtManifestError("TensorRT engine manifest CUDA runtime "
                                       "version does not match runtime");
    }
    if (manifest.compute_capability_major != runtime.compute_capability_major
        || manifest.compute_capability_minor
               != runtime.compute_capability_minor) {
        throw SsvTensorRtManifestError("TensorRT engine manifest compute "
                                       "capability does not match device");
    }
}

} // namespace

SsvTensorRtEngineManifest ssv_tensorrt_manifest_load_and_validate(
    const std::filesystem::path &manifest_path,
    std::span<const std::byte> engine_bytes,
    const SsvTensorRtRuntimeDescriptor &runtime)
{
    std::ifstream input(manifest_path, std::ios::binary);
    if (!input) {
        throw SsvTensorRtManifestError(
            "TensorRT engine manifest not found: " + manifest_path.string());
    }

    try {
        const auto root
            = nlohmann::json::parse(std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>());
        require_object_keys(root,
            "TensorRT engine manifest",
            {"schema", "schema_version", "engine", "wrapper"});
        if (required_string(root, "schema", "TensorRT engine manifest")
                != MANIFEST_SCHEMA
            || required_integer(
                   root, "schema_version", "TensorRT engine manifest")
                   != MANIFEST_SCHEMA_VERSION) {
            throw SsvTensorRtManifestError(
                "unsupported TensorRT engine manifest schema");
        }

        const auto &engine = root.at("engine");
        const auto &wrapper = root.at("wrapper");
        const auto &compute_capability = engine.at("compute_capability");
        require_object_keys(engine,
            "TensorRT engine manifest engine",
            {"sha256",
                "precision",
                "tensorrt_version",
                "cuda_runtime_version",
                "compute_capability"});
        require_object_keys(compute_capability,
            "TensorRT engine manifest engine.compute_capability",
            {"major", "minor"});
        require_object_keys(wrapper,
            "TensorRT engine manifest wrapper",
            {"sha256",
                "contract",
                "source_sha256",
                "tool_version",
                "model_family",
                "output_format",
                "input"});

        SsvTensorRtEngineManifest manifest;
        manifest.engine_sha256 = required_string(
            engine, "sha256", "TensorRT engine manifest engine");
        manifest.wrapper_sha256 = required_string(
            wrapper, "sha256", "TensorRT engine manifest wrapper");
        require_hash(
            "TensorRT engine manifest engine.sha256", manifest.engine_sha256);
        require_hash(
            "TensorRT engine manifest wrapper.sha256", manifest.wrapper_sha256);
        if (manifest.engine_sha256 != sha256(engine_bytes)) {
            throw SsvTensorRtManifestError(
                "TensorRT engine SHA-256 does not match manifest");
        }

        manifest.precision = parse_precision(required_string(
            engine, "precision", "TensorRT engine manifest engine"));
        manifest.tensorrt_version = required_string(
            engine, "tensorrt_version", "TensorRT engine manifest engine");
        manifest.cuda_runtime_version = required_integer(
            engine, "cuda_runtime_version", "TensorRT engine manifest engine");
        manifest.compute_capability_major = required_integer(compute_capability,
            "major",
            "TensorRT engine manifest engine.compute_capability");
        manifest.compute_capability_minor = required_integer(compute_capability,
            "minor",
            "TensorRT engine manifest engine.compute_capability");
        manifest.input = parse_input(wrapper.at("input"));

        const auto contract = required_string(
            wrapper, "contract", "TensorRT engine manifest wrapper");
        if (contract != "rgba_u8_nhwc_v1") {
            throw SsvTensorRtManifestError(
                "TensorRT engine manifest wrapper.contract must be "
                "rgba_u8_nhwc_v1");
        }
        const auto source_sha256 = required_string(
            wrapper, "source_sha256", "TensorRT engine manifest wrapper");
        const auto tool_version = required_string(
            wrapper, "tool_version", "TensorRT engine manifest wrapper");
        const auto model_family = required_string(
            wrapper, "model_family", "TensorRT engine manifest wrapper");
        const auto output_format = required_string(
            wrapper, "output_format", "TensorRT engine manifest wrapper");
        require_hash(
            "TensorRT engine manifest wrapper.source_sha256", source_sha256);

        manifest.wrapper_properties = {
            {"ssv.wrapper.channel_rule", "drop_alpha_keep_rgb"},
            {"ssv.wrapper.contract", contract},
            {"ssv.wrapper.dtype", "uint8"},
            {"ssv.wrapper.height", std::to_string(manifest.input.shape[1])},
            {"ssv.wrapper.layout", "NHWC"},
            {"ssv.wrapper.model_family", model_family},
            {"ssv.wrapper.normalization", "divide_by_255"},
            {"ssv.wrapper.output_format", output_format},
            {"ssv.wrapper.source_sha256", source_sha256},
            {"ssv.wrapper.tool", "ssv.prepare_wrapper"},
            {"ssv.wrapper.tool_version", tool_version},
            {"ssv.wrapper.width", std::to_string(manifest.input.shape[2])},
        };
        require_runtime_match(manifest, runtime);
        return manifest;
    } catch (const SsvTensorRtManifestError &) {
        throw;
    } catch (const std::exception &error) {
        throw SsvTensorRtManifestError(
            "invalid TensorRT engine manifest: " + std::string(error.what()));
    }
}

void ssv_tensorrt_manifest_apply(
    const SsvTensorRtEngineManifest &manifest, ModelMetadata &metadata)
{
    if (metadata.inputs.size() != 1) {
        throw SsvTensorRtManifestError(
            "TensorRT engine must have exactly one input");
    }
    if (!(metadata.inputs.front().name == manifest.input.name
            && metadata.inputs.front().dtype == manifest.input.dtype
            && metadata.inputs.front().shape == manifest.input.shape
            && metadata.inputs.front().layout == manifest.input.layout)) {
        throw SsvTensorRtManifestError(
            "TensorRT engine input does not match manifest");
    }
    metadata.properties = manifest.wrapper_properties;
}

} // namespace ssv::infer
