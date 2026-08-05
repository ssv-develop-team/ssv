#pragma once

#include "ssv_log_sink.hpp"

namespace ssv {

class SsvStderrLogSink final : public SsvLogSink {
public:
    SsvLogSubmitResult submit(
        SsvEncodedLogRecord record) noexcept override;

    SsvLogIoResult flush(
        std::chrono::steady_clock::time_point deadline) noexcept override;

    SsvLogIoResult close(
        std::chrono::steady_clock::time_point deadline) noexcept override;

private:
    bool closed_ = false;
    SsvLogSinkStats stats_;
};

} // namespace ssv
