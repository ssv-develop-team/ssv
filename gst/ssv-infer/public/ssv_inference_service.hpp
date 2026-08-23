#pragma once

#include "ssv_config.hpp"
#include "ssv_meta.hpp"
#include "ssv_model_contract.hpp"

#include <glib-object.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

G_BEGIN_DECLS

#define SSV_TYPE_INFERENCE_SERVICE (ssv_inference_service_get_type())
G_DECLARE_FINAL_TYPE(
    SsvInferenceService,
    ssv_inference_service,
    SSV,
    INFERENCE_SERVICE,
    GObject)

G_END_DECLS

namespace ssv::infer {

struct SsvInferenceServiceUnref {
    void operator()(SsvInferenceService *service) const noexcept;
};

using SsvInferenceServicePtr =
    std::unique_ptr<SsvInferenceService, SsvInferenceServiceUnref>;

class SsvInferenceServiceError : public std::runtime_error {
public:
    SsvInferenceServiceError(std::string stage, std::string message);

    [[nodiscard]] const std::string &stage() const noexcept;

private:
    std::string stage_;
};

struct SsvInferenceRequest {
    std::uint64_t frame_id = 0;
    std::string source_id;
    std::shared_ptr<const SsvAnalysisFrame> analysis_frame;
};

enum class SsvInferenceSubmissionStatus {
    Completed,
    Replaced,
    Cancelled,
    Failed,
};

struct SsvInferenceSubmissionResult {
    SsvInferenceSubmissionStatus status;
    SsvDetectionFrame detections;
    std::string error;
};

struct SsvInferenceStats {
    std::uint64_t submitted = 0;
    std::uint64_t completed = 0;
    std::uint64_t replaced = 0;
    std::uint64_t cancelled = 0;
    std::uint64_t failed = 0;
    std::uint64_t in_flight = 0;
    std::uint64_t pending = 0;
    std::uint64_t max_in_flight = 0;
    std::uint64_t max_pending = 0;
    SsvAnalysisFramePoolStats analysis_frames;
};

struct SsvLatencyPercentiles {
    // Empty percentiles are rendered as "unmeasured" by runtime observers.
    std::optional<std::uint64_t> p50_us;
    std::optional<std::uint64_t> p95_us;
};

struct SsvInferenceStatsWindow {
    std::uint64_t received = 0;
    std::uint64_t dropped = 0;
    std::uint64_t completed = 0;
    double completed_fps = 0.0;
    std::uint64_t longest_result_gap_us = 0;
    SsvLatencyPercentiles queue;
    SsvLatencyPercentiles decode;
    SsvLatencyPercentiles color_resize;
    SsvLatencyPercentiles normalize_layout;
    SsvLatencyPercentiles backend_h2d;
    SsvLatencyPercentiles backend_execution;
    SsvLatencyPercentiles backend_d2h;
    SsvLatencyPercentiles backend_unattributed;
    SsvLatencyPercentiles postprocess;
    SsvLatencyPercentiles total;
};

struct SsvInferenceProviderFallbackSnapshot {
    std::string provider;
    std::string resolved_provider_chain;
    std::string stage;
    std::string reason;
};

struct SsvInferenceRuntimeSnapshot {
    std::string provider_chain;
    std::string provider_device;
    std::string precision;
    std::string model_hash;
    std::string input_contract;
    std::string cache_status;
    std::vector<SsvInferenceProviderFallbackSnapshot> fallbacks;
};

[[nodiscard]] SsvInferenceServicePtr ssv_inference_service_create(
    const ssv::SsvInferenceConfig &config);

[[nodiscard]] SsvInferenceSubmissionResult ssv_inference_service_submit(
    SsvInferenceService *service,
    SsvInferenceRequest request);

[[nodiscard]] std::shared_ptr<const SsvAnalysisFrame>
ssv_inference_service_create_analysis_frame(
    SsvInferenceService *service,
    GstBuffer *buffer,
    const GstVideoInfo &video_info,
    PreprocessTransform transform,
    SsvFrameTiming timing);

void ssv_inference_service_cancel(
    SsvInferenceService *service,
    std::string_view source_id) noexcept;

void ssv_inference_service_stop(SsvInferenceService *service) noexcept;

[[nodiscard]] bool ssv_inference_service_is_running(
    SsvInferenceService *service) noexcept;

[[nodiscard]] SsvInferenceStats ssv_inference_service_stats(
    SsvInferenceService *service) noexcept;

[[nodiscard]] SsvInferenceStatsWindow
ssv_inference_service_take_stats_window(SsvInferenceService *service);

[[nodiscard]] SsvInferenceRuntimeSnapshot
ssv_inference_service_runtime_snapshot(SsvInferenceService *service);

[[nodiscard]] SsvModelContract ssv_inference_service_model_contract(
    SsvInferenceService *service);

void ssv_inference_service_update_source_geometry(
    SsvInferenceService *service,
    std::string_view source_id,
    int source_width,
    int source_height);

[[nodiscard]] PreprocessTransform
ssv_inference_service_preprocess_transform(
    SsvInferenceService *service,
    std::string_view source_id);

} // namespace ssv::infer
