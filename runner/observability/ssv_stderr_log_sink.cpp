#include "ssv_stderr_log_sink.hpp"

#include <cerrno>
#include <cstddef>

#include <unistd.h>

namespace ssv {

SsvLogSubmitResult SsvStderrLogSink::submit(
    SsvEncodedLogRecord record) noexcept
{
    if (closed_) {
        ++stats_.write_failed_records;
        return SsvLogSubmitResult::Failed;
    }

    std::size_t written = 0;
    while (written < record.bytes.size()) {
        const auto result = ::write(
            STDERR_FILENO,
            record.bytes.data() + written,
            record.bytes.size() - written);
        if (result < 0) {
            if (errno == EINTR)
                continue;
            ++stats_.write_failed_records;
            return SsvLogSubmitResult::Failed;
        }
        if (result == 0) {
            ++stats_.write_failed_records;
            return SsvLogSubmitResult::Failed;
        }
        written += static_cast<std::size_t>(result);
    }

    ++stats_.written_records;
    return SsvLogSubmitResult::Accepted;
}

SsvLogIoResult SsvStderrLogSink::flush(
    std::chrono::steady_clock::time_point) noexcept
{
    return {SsvLogIoStatus::Completed, stats_};
}

SsvLogIoResult SsvStderrLogSink::close(
    std::chrono::steady_clock::time_point) noexcept
{
    closed_ = true;
    return {SsvLogIoStatus::Completed, stats_};
}

} // namespace ssv
