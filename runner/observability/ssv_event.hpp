#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace ssv {

enum class SsvExitCode : int;

enum class SsvEventSeverity {
    Info,
    Warning,
    Error,
    Fatal,
};

enum class SsvNativeDiagnosticSeverity {
    Warning,
    Error,
};

struct SsvEventContext {
    std::string source_id = "unresolved";
    std::optional<std::uint32_t> run_attempt_id;
};

struct SsvRuntimeResolvedEvent {
    std::string decoder;
    std::string va_device;
    std::string va_driver;
    std::string decode_memory;
    std::string vpp;
    std::string display_backend;
    std::string egl_renderer;
    std::string provider_chain;
    std::string provider_device;
    std::string precision;
    std::string model_hash;
    std::string input_contract;
    std::string cache_status;
};

struct SsvAccelerationFallbackEvent {
    std::string from;
    std::string to;
    std::string stage;
    std::string reason;
};

struct SsvBufferContractFailedEvent {
    std::string stage;
    std::string expected_caps;
    std::string expected_allocator;
    std::string expected_memory;
    std::string actual_caps;
    std::string actual_allocator;
    std::string actual_memory;
    std::string reason;
};

struct SsvLatencyPercentiles {
    // Empty values are serialized as "unmeasured", never as zero.
    std::optional<std::chrono::microseconds> p50;
    std::optional<std::chrono::microseconds> p95;
};

struct SsvInferenceStatsEvent {
    std::uint64_t received = 0;
    std::uint64_t dropped = 0;
    std::uint64_t completed = 0;
    double completed_fps = 0.0;
    std::chrono::microseconds longest_result_gap {0};
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

struct SsvFatalErrorEvent {
    SsvExitCode exit_code;
    std::string stage;
    std::string error;
};

struct SsvNativeDiagnosticEvent {
    std::string origin;
    SsvNativeDiagnosticSeverity source_severity =
        SsvNativeDiagnosticSeverity::Warning;
    std::string domain;
    int code = 0;
    std::string message;
    std::optional<std::string> debug;
};

using SsvEventPayload = std::variant<
    SsvRuntimeResolvedEvent,
    SsvAccelerationFallbackEvent,
    SsvBufferContractFailedEvent,
    SsvInferenceStatsEvent,
    SsvFatalErrorEvent,
    SsvNativeDiagnosticEvent>;

struct SsvEvent {
    SsvEventContext context;
    SsvEventPayload payload;
};

} // namespace ssv
