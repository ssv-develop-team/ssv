#include "model/ssv_model_contract_internal.hpp"
#include "backends/tensorrt/ssv_tensorrt_manifest.hpp"

#include <glib.h>
#include <nlohmann/json.hpp>

#include <cassert>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <system_error>

#include <unistd.h>

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory()
    {
        char path[] = "/tmp/ssv-tensorrt-manifest-test-XXXXXX";
        const char *created = mkdtemp(path);
        assert(created != nullptr);
        path_ = created;
    }

    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path &path() const { return path_; }

private:
    std::filesystem::path path_;
};

std::string sha256(std::span<const std::byte> bytes)
{
    gchar *digest = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
        reinterpret_cast<const guchar *>(bytes.data()),
        bytes.size());
    assert(digest != nullptr);
    std::string result(digest);
    g_free(digest);
    return result;
}

nlohmann::json valid_manifest(std::span<const std::byte> engine_bytes)
{
    return {
        {"schema", "ssv.tensorrt-engine-manifest"},
        {"schema_version", 1},
        {"engine",
            {
                {"sha256", sha256(engine_bytes)},
                {"precision", "fp16"},
                {"tensorrt_version", "11.1.0.106"},
                {"cuda_runtime_version", 13020},
                {"compute_capability",
                    {
                        {"major", 8},
                        {"minor", 9},
                    }},
            }},
        {"wrapper",
            {
                {"sha256", std::string(64, 'b')},
                {"contract", "rgba_u8_nhwc_v1"},
                {"source_sha256", std::string(64, 'a')},
                {"tool_version", "1.0.0"},
                {"model_family", "yolo"},
                {"output_format", "yolov8"},
                {"input",
                    {
                        {"name", "images_rgba"},
                        {"dtype", "uint8"},
                        {"layout", "NHWC"},
                        {"shape", {1, 2, 3, 4}},
                    }},
            }},
    };
}

void write_manifest(
    const std::filesystem::path &path, const nlohmann::json &manifest)
{
    std::ofstream(path) << manifest.dump(2) << '\n';
}

ssv::infer::SsvTensorRtRuntimeDescriptor runtime_descriptor()
{
    return {
        .tensorrt_version = "11.1.0.106",
        .cuda_runtime_version = 13020,
        .compute_capability_major = 8,
        .compute_capability_minor = 9,
    };
}

void expect_manifest_error(nlohmann::json manifest_json,
    std::span<const std::byte> engine_bytes,
    std::string_view expected)
{
    TemporaryDirectory temporary;
    const auto manifest_path = temporary.path() / "model.engine.json";
    write_manifest(manifest_path, manifest_json);
    try {
        static_cast<void>(ssv::infer::ssv_tensorrt_manifest_load_and_validate(
            manifest_path, engine_bytes, runtime_descriptor()));
        assert(false && "invalid TensorRT manifest was accepted");
    } catch (const ssv::infer::SsvTensorRtManifestError &error) {
        assert(std::string(error.what()).find(expected) != std::string::npos);
    }
}

void test_valid_manifest_supplies_the_wrapper_contract()
{
    TemporaryDirectory temporary;
    const std::string engine_bytes = "engine-bytes";
    const auto engine_view = std::as_bytes(std::span(engine_bytes));
    const auto manifest_path = temporary.path() / "model.engine.json";

    write_manifest(manifest_path, valid_manifest(engine_view));

    const auto manifest = ssv::infer::ssv_tensorrt_manifest_load_and_validate(
        manifest_path, engine_view, runtime_descriptor());

    ssv::infer::ModelMetadata metadata;
    metadata.inputs.push_back({
        .name = "images_rgba",
        .dtype = ssv::infer::DataType::Uint8,
        .shape = {1, 2, 3, 4},
        .layout = ssv::infer::TensorLayout::Nhwc,
    });
    metadata.outputs.push_back({
        .name = "output0",
        .dtype = ssv::infer::DataType::Float32,
        .shape = {1, 1, 6},
    });
    ssv::infer::ssv_tensorrt_manifest_apply(manifest, metadata);

    const auto contract = ssv::infer::ssv_model_contract_validate(metadata,
        ssv::infer::ModelFamily::Yolo,
        ssv::infer::OutputFormat::YoloV8);
    assert(contract.width == 3);
    assert(contract.height == 2);
    assert(contract.input_bytes == 24);
    assert(manifest.precision == ssv::SsvPrecision::Fp16);
    assert(manifest.wrapper_sha256 == std::string(64, 'b'));
}

void test_rejects_unknown_manifest_keys()
{
    const std::string engine_bytes = "engine-bytes";
    const auto engine_view = std::as_bytes(std::span(engine_bytes));
    auto manifest = valid_manifest(engine_view);
    manifest["legacy"] = true;

    expect_manifest_error(
        std::move(manifest), engine_view, "unknown key: legacy");
}

void test_rejects_wrong_manifest_field_types()
{
    const std::string engine_bytes = "engine-bytes";
    const auto engine_view = std::as_bytes(std::span(engine_bytes));
    auto manifest = valid_manifest(engine_view);
    manifest["schema_version"] = 1.0;

    expect_manifest_error(
        std::move(manifest), engine_view, "schema_version must be an integer");
}

void test_rejects_unsupported_wrapper_contract()
{
    const std::string engine_bytes = "engine-bytes";
    const auto engine_view = std::as_bytes(std::span(engine_bytes));
    auto manifest = valid_manifest(engine_view);
    manifest["wrapper"]["contract"] = "legacy_rgba";

    expect_manifest_error(std::move(manifest),
        engine_view,
        "wrapper.contract must be rgba_u8_nhwc_v1");
}

void test_rejects_engine_identity_mismatches()
{
    const std::string engine_bytes = "engine-bytes";
    const auto engine_view = std::as_bytes(std::span(engine_bytes));

    auto wrong_hash = valid_manifest(engine_view);
    wrong_hash["engine"]["sha256"] = std::string(64, 'c');
    expect_manifest_error(
        std::move(wrong_hash), engine_view, "SHA-256 does not match");

    auto wrong_tensorrt = valid_manifest(engine_view);
    wrong_tensorrt["engine"]["tensorrt_version"] = "11.0.0.1";
    expect_manifest_error(
        std::move(wrong_tensorrt), engine_view, "TensorRT version");

    auto wrong_cuda = valid_manifest(engine_view);
    wrong_cuda["engine"]["cuda_runtime_version"] = 12080;
    expect_manifest_error(
        std::move(wrong_cuda), engine_view, "CUDA runtime version");

    auto wrong_device = valid_manifest(engine_view);
    wrong_device["engine"]["compute_capability"]["minor"] = 6;
    expect_manifest_error(
        std::move(wrong_device), engine_view, "compute capability");

    auto invalid_precision = valid_manifest(engine_view);
    invalid_precision["engine"]["precision"] = "int8";
    expect_manifest_error(
        std::move(invalid_precision), engine_view, "precision");
}

void test_rejects_engine_input_mismatch()
{
    TemporaryDirectory temporary;
    const std::string engine_bytes = "engine-bytes";
    const auto engine_view = std::as_bytes(std::span(engine_bytes));
    const auto manifest_path = temporary.path() / "model.engine.json";
    write_manifest(manifest_path, valid_manifest(engine_view));
    const auto manifest = ssv::infer::ssv_tensorrt_manifest_load_and_validate(
        manifest_path, engine_view, runtime_descriptor());

    ssv::infer::ModelMetadata metadata;
    metadata.inputs.push_back({
        .name = "images_rgba",
        .dtype = ssv::infer::DataType::Uint8,
        .shape = {1, 3, 2, 4},
        .layout = ssv::infer::TensorLayout::Nhwc,
    });
    try {
        ssv::infer::ssv_tensorrt_manifest_apply(manifest, metadata);
        assert(false && "mismatched TensorRT engine input was accepted");
    } catch (const ssv::infer::SsvTensorRtManifestError &error) {
        assert(std::string(error.what()).find("input does not match")
               != std::string::npos);
    }
}

} // namespace

int main()
{
    test_valid_manifest_supplies_the_wrapper_contract();
    test_rejects_unknown_manifest_keys();
    test_rejects_wrong_manifest_field_types();
    test_rejects_unsupported_wrapper_contract();
    test_rejects_engine_identity_mismatches();
    test_rejects_engine_input_mismatch();
    return 0;
}
