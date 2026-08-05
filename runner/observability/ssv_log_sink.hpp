#pragma once

#include "ssv_event.hpp"

#include <chrono>
#include <cstdint>
#include <string>

namespace ssv {

struct SsvEncodedLogRecord {
    std::string bytes;
    std::uint64_t sequence = 0;
    std::chrono::system_clock::time_point emitted_at;
    SsvEventSeverity severity = SsvEventSeverity::Info;
    bool force_flush = false;
};

enum class SsvLogSubmitResult {
    Accepted,
    DroppedBackpressure,
    Failed,
};

enum class SsvLogIoStatus {
    Completed,
    TimedOut,
    Failed,
};

struct SsvLogSinkStats {
    std::uint64_t written_records = 0;
    std::uint64_t async_dropped_records = 0;
    std::uint64_t write_failed_records = 0;
};

struct SsvLogIoResult {
    SsvLogIoStatus status = SsvLogIoStatus::Completed;
    SsvLogSinkStats stats;
};

class SsvLogSink {
public:
    virtual ~SsvLogSink() = default;

    virtual SsvLogSubmitResult submit(
        SsvEncodedLogRecord record) noexcept = 0;

    virtual SsvLogIoResult flush(
        std::chrono::steady_clock::time_point deadline) noexcept = 0;

    virtual SsvLogIoResult close(
        std::chrono::steady_clock::time_point deadline) noexcept = 0;
};

} // namespace ssv
