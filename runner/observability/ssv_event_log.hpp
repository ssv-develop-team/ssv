#pragma once

#include "ssv_event.hpp"
#include "ssv_log_sink.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

namespace ssv {

enum class SsvEventLogFormat {
    Structured,
    Pretty,
};

struct SsvEventLogOptions {
    SsvEventSeverity minimum_severity = SsvEventSeverity::Info;
    std::size_t max_record_bytes = 16 * 1024;
    std::chrono::milliseconds fatal_flush_timeout {250};
    std::chrono::milliseconds shutdown_timeout {1000};
    SsvEventLogFormat format = SsvEventLogFormat::Structured;
};

struct SsvEventLogStats {
    std::uint64_t emit_calls = 0;
    std::uint64_t filtered = 0;
    std::uint64_t rejected_after_close = 0;
    std::uint64_t encode_failed = 0;
    std::uint64_t accepted_by_sink = 0;
    std::uint64_t submit_dropped = 0;
    std::uint64_t submit_failed = 0;
    std::uint64_t truncated = 0;
    std::uint64_t written_records = 0;
    std::uint64_t async_dropped_records = 0;
    std::uint64_t write_failed_records = 0;
    std::uint64_t emergency_write_attempts = 0;
    std::uint64_t emergency_write_failures = 0;
    SsvLogIoStatus final_flush_status = SsvLogIoStatus::Completed;
    SsvLogIoStatus final_close_status = SsvLogIoStatus::Completed;
};

class SsvEventLog {
public:
    static std::unique_ptr<SsvEventLog> create(
        SsvEventLogOptions options,
        std::unique_ptr<SsvLogSink> sink);

    ~SsvEventLog() noexcept;

    SsvEventLog(const SsvEventLog &) = delete;
    SsvEventLog &operator=(const SsvEventLog &) = delete;

    void emit(SsvEvent event) noexcept;
    [[nodiscard]] SsvEventLogStats close() noexcept;

private:
    enum class State {
        Open,
        Closing,
        Closed,
    };

    SsvEventLog(
        SsvEventLogOptions options,
        std::unique_ptr<SsvLogSink> sink) noexcept;

    SsvEventLogOptions options_;
    std::unique_ptr<SsvLogSink> sink_;
    std::mutex mutex_;
    State state_ = State::Open;
    std::uint64_t next_sequence_ = 1;
    SsvEventLogStats stats_;
};

} // namespace ssv
