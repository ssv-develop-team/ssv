#include "ssv_inference_service.hpp"
#include "backends/onnxruntime/ssv_provider_resolver.hpp"
#include "backends/onnxruntime/ssv_session_pool.hpp"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

using Bytes = std::vector<std::uint8_t>;

class TemporaryDirectory {
public:
    TemporaryDirectory()
    {
        char path[] = "/tmp/ssv-provider-test-XXXXXX";
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

void append_varint(Bytes &output, std::uint64_t value)
{
    while (value >= 0x80U) {
        output.push_back(static_cast<std::uint8_t>(value) | 0x80U);
        value >>= 7U;
    }
    output.push_back(static_cast<std::uint8_t>(value));
}

void append_tag(Bytes &output, int field, int wire_type)
{
    append_varint(output,
        static_cast<std::uint64_t>((field << 3) | wire_type));
}

void append_integer(Bytes &output, int field, std::uint64_t value)
{
    append_tag(output, field, 0);
    append_varint(output, value);
}

void append_bytes(Bytes &output, int field, std::span<const std::uint8_t> value)
{
    append_tag(output, field, 2);
    append_varint(output, value.size());
    output.insert(output.end(), value.begin(), value.end());
}

void append_string(Bytes &output, int field, std::string_view value)
{
    append_bytes(output, field,
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t *>(value.data()),
            value.size()));
}

Bytes tensor_shape(const std::vector<std::int64_t> &shape)
{
    Bytes result;
    for (const auto dimension : shape) {
        Bytes encoded_dimension;
        append_integer(encoded_dimension, 1,
            static_cast<std::uint64_t>(dimension));
        append_bytes(result, 1, encoded_dimension);
    }
    return result;
}

Bytes tensor_type(int element_type, const std::vector<std::int64_t> &shape)
{
    Bytes tensor;
    append_integer(tensor, 1, element_type);
    const auto encoded_shape = tensor_shape(shape);
    append_bytes(tensor, 2, encoded_shape);

    Bytes type;
    append_bytes(type, 1, tensor);
    return type;
}

Bytes value_info(
    std::string_view name,
    int element_type,
    const std::vector<std::int64_t> &shape)
{
    Bytes result;
    append_string(result, 1, name);
    const auto type = tensor_type(element_type, shape);
    append_bytes(result, 2, type);
    return result;
}

Bytes integer_attribute(std::string_view name, std::int64_t value)
{
    Bytes result;
    append_string(result, 1, name);
    append_integer(result, 3, static_cast<std::uint64_t>(value));
    append_integer(result, 20, 2);
    return result;
}

Bytes integers_attribute(
    std::string_view name,
    const std::vector<std::int64_t> &values)
{
    Bytes result;
    append_string(result, 1, name);
    for (const auto value : values)
        append_integer(result, 8, static_cast<std::uint64_t>(value));
    append_integer(result, 20, 7);
    return result;
}

Bytes node(
    std::span<const std::string_view> inputs,
    std::string_view output,
    std::string_view name,
    std::string_view operation,
    std::span<const Bytes> attributes)
{
    Bytes result;
    for (const auto input : inputs)
        append_string(result, 1, input);
    append_string(result, 2, output);
    append_string(result, 3, name);
    append_string(result, 4, operation);
    for (const auto &attribute : attributes)
        append_bytes(result, 5, attribute);
    return result;
}

Bytes make_raw_model()
{
    const auto cast_to_float = integer_attribute("to", 1);
    const std::vector<Bytes> cast_attributes {cast_to_float};
    const std::vector<std::string_view> cast_inputs {"images"};
    const auto cast = node(cast_inputs,
        "float_rgba", "cast_input", "Cast", cast_attributes);

    const auto axes = integers_attribute("axes", {1});
    const auto keepdims = integer_attribute("keepdims", 0);
    const std::vector<Bytes> mean_attributes {axes, keepdims};
    const std::vector<std::string_view> mean_inputs {"float_rgba"};
    const auto mean = node(mean_inputs,
        "output0", "reduce_channels", "ReduceMean", mean_attributes);

    Bytes graph;
    append_bytes(graph, 1, cast);
    append_bytes(graph, 1, mean);
    append_string(graph, 2, "ssv-provider-cpu-smoke");
    const auto input = value_info("images", 1, {1, 3, 1, 6});
    const auto output = value_info("output0", 1, {1, 1, 6});
    append_bytes(graph, 11, input);
    append_bytes(graph, 12, output);

    Bytes opset;
    append_integer(opset, 2, 13);

    Bytes model;
    append_integer(model, 1, 8);
    append_string(model, 2, "ssv-provider-test");
    append_bytes(model, 7, graph);
    append_bytes(model, 8, opset);

    return model;
}

std::vector<ssv::SsvProvider> chain(
    const ssv::infer::SsvProviderAttempt &attempt)
{
    std::vector<ssv::SsvProvider> result;
    for (const auto &registration : attempt.providers)
        result.push_back(registration.provider);
    return result;
}

std::string option(
    const ssv::infer::SsvProviderRegistration &registration,
    std::string_view key)
{
    for (const auto &[name, value] : registration.options) {
        if (name == key)
            return value;
    }
    return {};
}

ssv::infer::SsvProviderResolveRequest request_for(
    ssv::infer::SsvRuntimeProfile profile,
    std::vector<ssv::SsvProvider> available)
{
    ssv::infer::SsvProviderResolveRequest request;
    request.profile = profile;
    request.available_providers = std::move(available);
    request.logical_cpu_count = 12;
    return request;
}

void test_profile_chains_and_aliases()
{
    using enum ssv::SsvProvider;

    const std::vector<std::pair<
        ssv::infer::SsvRuntimeProfile,
        std::vector<ssv::SsvProvider>>>
        profiles {
            {ssv::infer::SsvRuntimeProfile::Cpu, {Cpu}},
            {ssv::infer::SsvRuntimeProfile::Nvidia,
                {TensorRt, Cuda, Cpu}},
            {ssv::infer::SsvRuntimeProfile::Intel, {OpenVino, Cpu}},
            {ssv::infer::SsvRuntimeProfile::Amd, {MiGraphX, Cpu}},
        };
    for (const auto &[profile, expected] : profiles) {
        const auto result = ssv::infer::ssv_provider_resolve(
            request_for(profile, expected), [](const auto &) {});
        assert(chain(result.active) == expected);
    }

    assert(ssv::infer::ssv_provider_parse("tensorrt") == TensorRt);
    assert(ssv::infer::ssv_provider_parse("cuda") == Cuda);
    assert(ssv::infer::ssv_provider_parse("openvino") == OpenVino);
    assert(ssv::infer::ssv_provider_parse("migraphx") == MiGraphX);
    assert(ssv::infer::ssv_provider_parse("cpu") == Cpu);
    try {
        static_cast<void>(ssv::infer::ssv_provider_parse("rocm"));
        assert(false && "ROCm alias was accepted");
    } catch (const ssv::infer::SsvProviderResolutionError &) {
    }
}

void test_auto_fallback_records_every_reason()
{
    using enum ssv::SsvProvider;
    using enum ssv::infer::SsvProviderFailureStage;

    auto request = request_for(
        ssv::infer::SsvRuntimeProfile::Nvidia, {Cuda, Cpu});
    int attempts = 0;
    const auto result = ssv::infer::ssv_provider_resolve(
        request, [&](const auto &) {
            ++attempts;
            if (attempts == 1) {
                throw ssv::infer::SsvProviderAttemptError(
                    Cuda, Append, "CUDA provider library could not be loaded");
            }
        });

    assert(attempts == 2);
    assert(chain(result.active) == std::vector<ssv::SsvProvider>({Cpu}));
    assert(result.active.precision == ssv::SsvPrecision::Fp32);
    assert(result.fallbacks.size() == 2);
    assert(result.fallbacks[0].provider == TensorRt);
    assert(result.fallbacks[0].stage == Availability);
    assert(result.fallbacks[1].provider == Cuda);
    assert(result.fallbacks[1].stage == Append);
    assert(result.fallbacks[1].reason.find("could not be loaded")
        != std::string::npos);
}

void test_auto_session_failure_retries_but_explicit_stops()
{
    using enum ssv::SsvProvider;
    using enum ssv::infer::SsvProviderFailureStage;

    int auto_attempts = 0;
    const auto automatic = ssv::infer::ssv_provider_resolve(
        request_for(ssv::infer::SsvRuntimeProfile::Nvidia,
            {TensorRt, Cuda, Cpu}),
        [&](const auto &) {
            ++auto_attempts;
            if (auto_attempts == 1) {
                throw ssv::infer::SsvProviderAttemptError(
                    std::nullopt, Session, "TensorRT session initialization failed");
            }
        });
    assert(auto_attempts == 2);
    assert(chain(automatic.active)
        == std::vector<ssv::SsvProvider>({Cuda, Cpu}));
    assert(automatic.fallbacks.size() == 1);
    assert(automatic.fallbacks.front().provider == TensorRt);
    assert(automatic.fallbacks.front().stage == Session);

    auto explicit_request = request_for(
        ssv::infer::SsvRuntimeProfile::Nvidia,
        {TensorRt, Cuda, Cpu});
    explicit_request.providers.mode = ssv::SsvProviderMode::Explicit;
    explicit_request.providers.order = {TensorRt, Cuda, Cpu};
    int explicit_attempts = 0;
    try {
        static_cast<void>(ssv::infer::ssv_provider_resolve(
            explicit_request, [&](const auto &) {
                ++explicit_attempts;
                throw ssv::infer::SsvProviderAttemptError(
                    TensorRt, Append, "append failed");
            }));
        assert(false && "explicit Provider failure was ignored");
    } catch (const ssv::infer::SsvProviderResolutionError &error) {
        assert(error.stage() == Append);
        assert(error.provider() == TensorRt);
    }
    assert(explicit_attempts == 1);
}

void test_explicit_provider_order_is_exact()
{
    using enum ssv::SsvProvider;

    auto request = request_for(
        ssv::infer::SsvRuntimeProfile::Nvidia, {Cuda, Cpu});
    request.providers.mode = ssv::SsvProviderMode::Explicit;
    request.providers.order = {Cuda};

    const auto result = ssv::infer::ssv_provider_resolve(
        request, [](const auto &) {});
    assert(chain(result.active) == std::vector<ssv::SsvProvider>({Cuda}));
    assert(result.active.disable_cpu_fallback);
}

void test_precision_provider_options_and_threads()
{
    using enum ssv::SsvProvider;

    auto intel_request = request_for(
        ssv::infer::SsvRuntimeProfile::Intel, {OpenVino, Cpu});
    intel_request.device_id = 3;
    const auto intel = ssv::infer::ssv_provider_resolve(
        intel_request,
        [](const auto &) {});
    assert(intel.active.precision == ssv::SsvPrecision::Fp16);
    assert(option(intel.active.providers.front(), "device_type")
        == "GPU.3");
    assert(option(intel.active.providers.front(), "precision") == "FP16");
    assert(intel.active.threading.intra_op_threads == 1);
    assert(intel.active.threading.inter_op_threads == 1);
    assert(!intel.active.threading.allow_spinning);

    auto cuda_request = request_for(
        ssv::infer::SsvRuntimeProfile::Nvidia, {Cuda, Cpu});
    const auto cuda = ssv::infer::ssv_provider_resolve(
        cuda_request, [](const auto &) {});
    assert(cuda.active.precision == ssv::SsvPrecision::Fp32);
    cuda_request.precision = ssv::SsvPrecision::Fp16;
    try {
        static_cast<void>(ssv::infer::ssv_provider_resolve(
            cuda_request, [](const auto &) {}));
        assert(false && "CUDA-only FP16 was accepted");
    } catch (const ssv::infer::SsvProviderResolutionError &error) {
        assert(error.provider() == Cuda);
        assert(error.stage()
            == ssv::infer::SsvProviderFailureStage::Configuration);
    }

    auto cpu_request = request_for(
        ssv::infer::SsvRuntimeProfile::Cpu, {Cpu});
    cpu_request.logical_cpu_count = 6;
    const auto cpu = ssv::infer::ssv_provider_resolve(
        cpu_request, [](const auto &) {});
    assert(cpu.active.precision == ssv::SsvPrecision::Fp32);
    assert(cpu.active.threading.intra_op_threads == 4);
    assert(cpu.active.threading.inter_op_threads == 1);
    assert(!cpu.active.threading.allow_spinning);

    cpu_request.cpu_threads = 3;
    const auto explicit_threads = ssv::infer::ssv_provider_resolve(
        cpu_request, [](const auto &) {});
    assert(explicit_threads.active.threading.intra_op_threads == 3);

    cpu_request.precision = ssv::SsvPrecision::Fp16;
    try {
        static_cast<void>(ssv::infer::ssv_provider_resolve(
            cpu_request, [](const auto &) {}));
        assert(false && "CPU-only FP16 was accepted");
    } catch (const ssv::infer::SsvProviderResolutionError &) {
    }
}

void test_device_identity_contract()
{
    assert(ssv::infer::ssv_drm_cache_identity_is_unambiguous(1, 0));
    assert(!ssv::infer::ssv_drm_cache_identity_is_unambiguous(2, 0));
    assert(!ssv::infer::ssv_drm_cache_identity_is_unambiguous(1, 1));

    const ssv::infer::SsvNvidiaDeviceDescriptor descriptor {
        "0000:01:00.0",
        "GPU-00112233-4455-6677-8899-aabbccddeeff",
        8,
        9,
        "610.74",
    };
    const auto identity =
        ssv::infer::ssv_make_nvidia_device_identity(descriptor);
    assert(identity.unavailable_reason.empty());
    assert(identity.value
        == "nvidia:pci=0000:01:00.0:uuid="
           "GPU-00112233-4455-6677-8899-aabbccddeeff:"
           "compute_capability=8.9:driver=610.74");

    auto incomplete = descriptor;
    incomplete.driver_version.clear();
    const auto unavailable =
        ssv::infer::ssv_make_nvidia_device_identity(incomplete);
    assert(unavailable.value.empty());
    assert(!unavailable.unavailable_reason.empty());
}

void test_session_pool_key_and_cache_namespace()
{
    using enum ssv::SsvProvider;
    using enum ssv::infer::SsvCacheStatus;

    const std::string model_hash(64, 'a');
    ssv::infer::SsvSessionKey key {
        model_hash,
        {Cuda, Cpu},
        0,
        ssv::SsvPrecision::Fp16,
    };

    ssv::infer::SsvSessionPool<int> pool;
    int creations = 0;
    const auto first = pool.acquire(key, [&] {
        return std::make_shared<int>(++creations);
    });
    const auto reused = pool.acquire(key, [&] {
        return std::make_shared<int>(++creations);
    });
    assert(!first.reused);
    assert(reused.reused);
    assert(first.session == reused.session);
    assert(creations == 1);

    auto other_device = key;
    other_device.device_id = 1;
    const auto separate = pool.acquire(other_device, [&] {
        return std::make_shared<int>(++creations);
    });
    assert(!separate.reused);
    assert(separate.session != first.session);

    TemporaryDirectory directory;
    ssv::SsvCacheConfig cache;
    cache.enabled = true;
    cache.directory = directory.path().string();
    ssv::infer::SsvCacheIdentity identity;
    identity.session_key = key;
    identity.provider_options = {
        {Cuda, {{"device_id", "0"}}},
        {Cpu, {}},
    };
    identity.dependency_signature = std::string(64, 'b');
    identity.device_identity = "gpu-uuid-a";
    identity.runtime_versions = {{"onnxruntime", "1.25.1"}};

    const auto miss = ssv::infer::ssv_cache_prepare(cache, identity, true);
    assert(miss.status == Miss);
    assert(std::filesystem::is_directory(miss.path));
    assert(std::filesystem::is_regular_file(miss.manifest_path));
    const auto hit = ssv::infer::ssv_cache_prepare(cache, identity, true);
    assert(hit.status == Hit);
    assert(hit.namespace_id == miss.namespace_id);

    auto other_identity = identity;
    other_identity.device_identity = "gpu-uuid-b";
    const auto other_device_cache =
        ssv::infer::ssv_cache_prepare(cache, other_identity, true);
    assert(other_device_cache.namespace_id != miss.namespace_id);

    other_identity = identity;
    other_identity.runtime_versions.front().second = "1.26.0";
    const auto other_version_cache =
        ssv::infer::ssv_cache_prepare(cache, other_identity, true);
    assert(other_version_cache.namespace_id != miss.namespace_id);

    other_identity = identity;
    other_identity.dependency_signature = std::string(64, 'c');
    const auto other_dependencies_cache =
        ssv::infer::ssv_cache_prepare(cache, other_identity, true);
    assert(other_dependencies_cache.namespace_id != miss.namespace_id);

    other_identity = identity;
    other_identity.dependency_signature.clear();
    const auto unidentified_dependencies =
        ssv::infer::ssv_cache_prepare(cache, other_identity, true);
    assert(unidentified_dependencies.status == Unavailable);
    assert(unidentified_dependencies.path.empty());

    std::ofstream(miss.manifest_path, std::ios::trunc) << "corrupt\n";
    const auto rebuilt = ssv::infer::ssv_cache_prepare(cache, identity, true);
    assert(rebuilt.status == Rebuilt);
    assert(rebuilt.namespace_id == miss.namespace_id);

    const auto unsupported =
        ssv::infer::ssv_cache_prepare(cache, identity, false);
    assert(unsupported.status == NotSupported);
    assert(unsupported.path.empty());
}

void test_cpu_execution_provider_smoke()
{
    const char *label_map = std::getenv("SSV_TEST_LABEL_MAP_PATH");
    assert(label_map != nullptr && label_map[0] != '\0');

    TemporaryDirectory directory;
    const auto model_path = directory.path() / "raw.onnx";
    const auto model = make_raw_model();
    std::ofstream output(model_path, std::ios::binary);
    output.write(reinterpret_cast<const char *>(model.data()),
        static_cast<std::streamsize>(model.size()));
    output.close();

    ssv::SsvInferenceConfig config;
    config.model.path = model_path.string();
    config.model.output_format = "yolo_nx6";
    config.model.label_map = label_map;
    config.model.preprocess = ssv::SsvPreprocessConfig {
        .color_order = ssv::SsvInputColorOrder::Rgb,
        .resize_mode = ssv::SsvResizeMode::Letterbox,
        .normalization = {
            .scale = 1.0F / 255.0F,
            .mean = {0.0F, 0.0F, 0.0F},
            .std = {1.0F, 1.0F, 1.0F},
        },
    };
    auto &runtime = std::get<ssv::SsvOnnxRuntimeConfig>(config.runtime);
    runtime.providers.mode = ssv::SsvProviderMode::Explicit;
    runtime.providers.order = {ssv::SsvProvider::Cpu};
    runtime.precision = ssv::SsvPrecision::Auto;
    runtime.cache.directory = (directory.path() / "cache").string();

    auto service = ssv::infer::ssv_inference_service_create(config);
    const auto snapshot =
        ssv::infer::ssv_inference_service_runtime_snapshot(service.get());
    assert(snapshot.provider_chain == "CPUExecutionProvider");
    assert(snapshot.precision == "fp32");
    assert(snapshot.cache_status == "not-supported");
    assert(snapshot.model_hash.size() == 64);
    assert(snapshot.input_contract == "float32[1,3,1,6]:NCHW");

    GstVideoInfo info;
    gst_video_info_init(&info);
    assert(gst_video_info_set_format(&info, GST_VIDEO_FORMAT_RGBA, 6, 1));
    GstBuffer *buffer = gst_buffer_new_allocate(nullptr, 24, nullptr);
    assert(buffer != nullptr);
    ssv::infer::SsvInferenceRequest request;
    request.frame_id = 1;
    request.source_id = "cpu-smoke";
    request.analysis_frame =
        ssv::infer::ssv_inference_service_create_analysis_frame(
            service.get(),
            buffer,
            info,
            {6, 1, 6, 1, 1.0F, 1.0F, 0, 0, 0, 0},
            {});
    gst_buffer_unref(buffer);
    auto result = ssv::infer::ssv_inference_service_submit(
        service.get(), std::move(request));
    assert(result.status
        == ssv::infer::SsvInferenceSubmissionStatus::Completed);
    assert(result.detections.frame_id == 1);
    assert(result.detections.detections.empty());
    result.detections.analysis_frame.reset();
    const auto stats = ssv::infer::ssv_inference_service_stats(service.get());
    assert(stats.analysis_frames.active_maps == 0);
    assert(stats.analysis_frames.outstanding_staging_leases == 0);
}

} // namespace

int main(int argc, char **argv)
{
    gst_init(&argc, &argv);
    test_profile_chains_and_aliases();
    test_auto_fallback_records_every_reason();
    test_auto_session_failure_retries_but_explicit_stops();
    test_explicit_provider_order_is_exact();
    test_precision_provider_options_and_threads();
    test_device_identity_contract();
    test_session_pool_key_and_cache_namespace();
    test_cpu_execution_provider_smoke();
    return 0;
}
