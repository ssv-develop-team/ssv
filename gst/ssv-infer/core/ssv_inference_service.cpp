#include "ssv_inference_service.hpp"

#include "core/ssv_inference_service_internal.hpp"
#include "core/ssv_latest_frame_scheduler.hpp"
#include "model/ssv_model_contract_internal.hpp"
#include "core/ssv_inference_config.hpp"
#include "core/ssv_inference_engine.hpp"
#include "core/ssv_inference_stats.hpp"
#include "backends/onnxruntime/ssv_provider_resolver.hpp"
#include "backends/onnxruntime/ssv_session_pool.hpp"

#include <exception>
#include <mutex>
#include <optional>
#include <stop_token>
#include <unordered_map>
#include <utility>

namespace ssv::infer {
class InferenceServiceImpl;
}

struct _SsvInferenceService {
    GObject parent;
    ssv::infer::InferenceServiceImpl *impl;
};

G_DEFINE_TYPE(SsvInferenceService, ssv_inference_service, G_TYPE_OBJECT)

namespace ssv::infer {
namespace {

constexpr std::size_t kAnalysisStagingCapacity = 3;

std::string_view precision_name(
    std::optional<ssv::SsvPrecision> precision) noexcept
{
    if (!precision)
        return "unknown";
    switch (*precision) {
    case ssv::SsvPrecision::Auto: return "auto";
    case ssv::SsvPrecision::Fp32: return "fp32";
    case ssv::SsvPrecision::Fp16: return "fp16";
    }
    return "unknown";
}

std::string provider_chain_text(
    const std::vector<ssv::SsvProvider> &providers)
{
    std::string chain;
    for (const auto provider : providers) {
        if (!chain.empty())
            chain += ',';
        chain += ssv_provider_runtime_name(provider);
    }
    return chain.empty() ? "unknown" : chain;
}

SsvInferenceRuntimeSnapshot make_runtime_snapshot(
    const BackendInfo &backend,
    std::string input_contract)
{
    SsvInferenceRuntimeSnapshot snapshot {
        .provider_chain = "unknown",
        .provider_device = "unknown",
        .precision = std::string(precision_name(
            backend.resolved_precision)),
        .model_hash = "unknown",
        .input_contract = std::move(input_contract),
        .cache_status = "disabled",
        .fallbacks = {},
    };

    const auto *onnx = std::get_if<OnnxRuntimeBackendInfo>(
        &backend.runtime);
    if (onnx != nullptr) {
        snapshot.provider_chain = provider_chain_text(
            onnx->active_provider_chain);
        if (!onnx->device_identity.empty())
            snapshot.provider_device = onnx->device_identity;
        if (!onnx->model_hash.empty())
            snapshot.model_hash = onnx->model_hash;
        snapshot.cache_status = ssv_cache_status_name(onnx->cache_status);
        snapshot.fallbacks.reserve(onnx->fallbacks.size());
        for (const auto &fallback : onnx->fallbacks) {
            snapshot.fallbacks.push_back({
                .provider = std::string(ssv_provider_runtime_name(
                    fallback.provider)),
                .resolved_provider_chain = snapshot.provider_chain,
                .stage = std::string(ssv_provider_failure_stage_name(
                    fallback.stage)),
                .reason = fallback.reason,
            });
        }
        return snapshot;
    }

    const auto *tensorrt = std::get_if<TensorRtEngineBackendInfo>(
        &backend.runtime);
    if (tensorrt == nullptr)
        return snapshot;

    snapshot.provider_chain = "TensorRTEngine";
    snapshot.provider_device = "device:"
        + std::to_string(backend.active_device_id)
        + "/compute_capability:";
    if (tensorrt->compute_capability_major >= 0
        && tensorrt->compute_capability_minor >= 0) {
        snapshot.provider_device +=
            std::to_string(tensorrt->compute_capability_major)
            + "."
            + std::to_string(tensorrt->compute_capability_minor);
    } else {
        snapshot.provider_device += "unknown";
    }
    if (!tensorrt->source_model_hash.empty())
        snapshot.model_hash = tensorrt->source_model_hash;
    return snapshot;
}

} // namespace

class InferenceServiceImpl {
public:
    InferenceServiceImpl(
        std::unique_ptr<InferenceBackend> backend,
        std::shared_ptr<SsvInferenceBufferAllocator> allocator)
        : engine_(std::move(backend), std::move(allocator))
        , scheduler_(std::make_unique<SsvLatestFrameScheduler>(
              [this](const SsvInferenceRequest &request,
                     std::stop_token stop_token) {
                  return engine_.run(request, stop_token);
              }))
    {
    }

    ~InferenceServiceImpl() { stop(); }

    void start(const InferenceConfig &config)
    {
        engine_.start(config);
        const auto &contract = engine_.model_contract();
        model_contract_ = contract;
        runtime_snapshot_ = make_runtime_snapshot(
            engine_.backend_info(), engine_.input_contract_description());
        analysis_frames_ = std::make_unique<SsvAnalysisFramePool>(
            contract.width,
            contract.height,
            kAnalysisStagingCapacity);
        try {
            scheduler_->start();
        } catch (...) {
            engine_.stop();
            throw;
        }
    }

    SsvInferenceSubmissionResult submit(SsvInferenceRequest request)
    {
        return scheduler_->submit(std::move(request));
    }

    std::shared_ptr<const SsvAnalysisFrame> create_analysis_frame(
        GstBuffer *buffer,
        const GstVideoInfo &video_info,
        PreprocessTransform transform,
        SsvFrameTiming timing)
    {
        if (!scheduler_->running() || !analysis_frames_)
            throw std::logic_error("inference service is stopped");
        return analysis_frames_->create(
            buffer, video_info, std::move(transform), timing);
    }

    void cancel(std::string_view source_id) noexcept
    {
        scheduler_->cancel(source_id);
    }

    void stop() noexcept
    {
        scheduler_->stop();
        engine_.stop();
    }

    bool running() const noexcept
    {
        return scheduler_->running();
    }

    SsvInferenceStats stats() const noexcept
    {
        auto result = scheduler_->stats();
        if (analysis_frames_)
            result.analysis_frames = analysis_frames_->stats();
        return result;
    }

    SsvInferenceStatsWindow take_stats_window()
    {
        return scheduler_->take_stats_window();
    }

    SsvInferenceRuntimeSnapshot runtime_snapshot() const
    {
        if (!runtime_snapshot_)
            throw std::logic_error("inference service is not started");
        return *runtime_snapshot_;
    }

    SsvModelContract model_contract() const
    {
        if (!model_contract_)
            throw std::logic_error("inference service is not started");
        return *model_contract_;
    }

    void update_source_geometry(
        std::string_view source_id,
        int source_width,
        int source_height)
    {
        if (!scheduler_->running() || !model_contract_)
            throw std::logic_error("inference service is stopped");
        if (source_id.empty())
            throw std::invalid_argument("source id must not be empty");
        std::lock_guard<std::mutex> lock(source_geometry_mutex_);
        source_transforms_.insert_or_assign(
            std::string(source_id),
            ssv_make_resize_transform(
                source_width,
                source_height,
                model_contract_->width,
                model_contract_->height,
                model_contract_->resize_mode));
    }

    PreprocessTransform preprocess_transform(
        std::string_view source_id) const
    {
        std::lock_guard<std::mutex> lock(source_geometry_mutex_);
        const auto found = source_transforms_.find(std::string(source_id));
        if (found == source_transforms_.end()) {
            throw std::logic_error(
                "source geometry has not been negotiated");
        }
        return found->second;
    }

private:
    InferenceEngine engine_;
    std::unique_ptr<SsvLatestFrameScheduler> scheduler_;
    std::unique_ptr<SsvAnalysisFramePool> analysis_frames_;
    std::optional<SsvModelContract> model_contract_;
    std::optional<SsvInferenceRuntimeSnapshot> runtime_snapshot_;
    mutable std::mutex source_geometry_mutex_;
    std::unordered_map<std::string, PreprocessTransform> source_transforms_;
};

SsvInferenceServiceError::SsvInferenceServiceError(
    std::string stage,
    std::string message)
    : std::runtime_error(std::move(message))
    , stage_(std::move(stage))
{
}

const std::string &SsvInferenceServiceError::stage() const noexcept
{
    return stage_;
}

void SsvInferenceServiceUnref::operator()(
    SsvInferenceService *service) const noexcept
{
    if (service != nullptr)
        g_object_unref(service);
}

SsvInferenceServicePtr ssv_inference_service_create(
    const ssv::SsvInferenceConfig &config)
{
    return ssv_inference_service_create_with_backend(config, {});
}

SsvInferenceServicePtr ssv_inference_service_create_with_backend(
    const ssv::SsvInferenceConfig &config,
    std::unique_ptr<InferenceBackend> backend,
    std::shared_ptr<SsvInferenceBufferAllocator> allocator)
{
    SsvInferenceServicePtr service(SSV_INFERENCE_SERVICE(
        g_object_new(SSV_TYPE_INFERENCE_SERVICE, nullptr)));
    auto impl = std::make_unique<InferenceServiceImpl>(
        std::move(backend), std::move(allocator));
    try {
        impl->start(make_inference_config(config));
    } catch (const SsvModelContractError &error) {
        throw SsvInferenceServiceError(
            "inference.model_contract", error.what());
    } catch (const std::exception &error) {
        throw SsvInferenceServiceError("inference.start", error.what());
    }
    service->impl = impl.release();
    return service;
}

SsvInferenceSubmissionResult ssv_inference_service_submit(
    SsvInferenceService *service,
    SsvInferenceRequest request)
{
    if (service == nullptr || service->impl == nullptr) {
        throw std::invalid_argument(
            "inference service must be created through its public factory");
    }
    return service->impl->submit(std::move(request));
}

std::shared_ptr<const SsvAnalysisFrame>
ssv_inference_service_create_analysis_frame(
    SsvInferenceService *service,
    GstBuffer *buffer,
    const GstVideoInfo &video_info,
    PreprocessTransform transform,
    SsvFrameTiming timing)
{
    if (service == nullptr || service->impl == nullptr) {
        throw std::invalid_argument(
            "inference service must be created through its public factory");
    }
    return service->impl->create_analysis_frame(
        buffer, video_info, std::move(transform), timing);
}

void ssv_inference_service_cancel(
    SsvInferenceService *service,
    std::string_view source_id) noexcept
{
    if (service != nullptr && service->impl != nullptr)
        service->impl->cancel(source_id);
}

void ssv_inference_service_stop(SsvInferenceService *service) noexcept
{
    if (service != nullptr && service->impl != nullptr)
        service->impl->stop();
}

bool ssv_inference_service_is_running(
    SsvInferenceService *service) noexcept
{
    return service != nullptr && service->impl != nullptr
        && service->impl->running();
}

SsvInferenceStats ssv_inference_service_stats(
    SsvInferenceService *service) noexcept
{
    if (service == nullptr || service->impl == nullptr)
        return {};
    return service->impl->stats();
}

SsvInferenceStatsWindow ssv_inference_service_take_stats_window(
    SsvInferenceService *service)
{
    if (service == nullptr || service->impl == nullptr)
        return {};
    return service->impl->take_stats_window();
}

SsvInferenceRuntimeSnapshot ssv_inference_service_runtime_snapshot(
    SsvInferenceService *service)
{
    if (service == nullptr || service->impl == nullptr) {
        throw std::invalid_argument(
            "inference service must be created through its public factory");
    }
    return service->impl->runtime_snapshot();
}

SsvModelContract ssv_inference_service_model_contract(
    SsvInferenceService *service)
{
    if (service == nullptr || service->impl == nullptr) {
        throw std::invalid_argument(
            "inference service must be created through its public factory");
    }
    return service->impl->model_contract();
}

void ssv_inference_service_update_source_geometry(
    SsvInferenceService *service,
    std::string_view source_id,
    int source_width,
    int source_height)
{
    if (service == nullptr || service->impl == nullptr) {
        throw std::invalid_argument(
            "inference service must be created through its public factory");
    }
    service->impl->update_source_geometry(
        source_id, source_width, source_height);
}

PreprocessTransform ssv_inference_service_preprocess_transform(
    SsvInferenceService *service,
    std::string_view source_id)
{
    if (service == nullptr || service->impl == nullptr) {
        throw std::invalid_argument(
            "inference service must be created through its public factory");
    }
    return service->impl->preprocess_transform(source_id);
}

} // namespace ssv::infer

static void ssv_inference_service_finalize(GObject *object)
{
    auto *service = SSV_INFERENCE_SERVICE(object);
    delete service->impl;
    service->impl = nullptr;
    G_OBJECT_CLASS(ssv_inference_service_parent_class)->finalize(object);
}

static void ssv_inference_service_class_init(SsvInferenceServiceClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = ssv_inference_service_finalize;
}

static void ssv_inference_service_init(SsvInferenceService *service)
{
    service->impl = nullptr;
}
