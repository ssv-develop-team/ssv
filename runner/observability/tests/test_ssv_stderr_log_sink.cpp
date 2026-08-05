#include "observability/ssv_stderr_log_sink.hpp"

#include <cassert>
#include <cerrno>
#include <chrono>
#include <string>

#include <unistd.h>

namespace {

struct WriteScript {
    bool enabled = false;
    std::size_t calls = 0;
    std::string captured;
};

WriteScript write_script;

} // namespace

extern "C" ssize_t __real_write(int, const void *, std::size_t);

extern "C" ssize_t __wrap_write(
    int file_descriptor,
    const void *buffer,
    std::size_t count)
{
    if (!write_script.enabled || file_descriptor != STDERR_FILENO)
        return __real_write(file_descriptor, buffer, count);

    ++write_script.calls;
    if (write_script.calls == 1) {
        errno = EINTR;
        return -1;
    }

    const auto bytes_written = write_script.calls == 2
        ? count / 2
        : count;
    write_script.captured.append(
        static_cast<const char *>(buffer), bytes_written);
    return static_cast<ssize_t>(bytes_written);
}

namespace {

void test_submit_writes_complete_record_and_close_keeps_stderr_open()
{
    int pipe_fds[2] {-1, -1};
    assert(::pipe(pipe_fds) == 0);
    const int saved_stderr = ::dup(STDERR_FILENO);
    assert(saved_stderr >= 0);
    assert(::dup2(pipe_fds[1], STDERR_FILENO) == STDERR_FILENO);

    ssv::SsvStderrLogSink sink;
    const auto submit = sink.submit({
        .bytes = "event=test value=first\n",
        .sequence = 1,
        .emitted_at = std::chrono::system_clock::now(),
        .severity = ssv::SsvEventSeverity::Info,
        .force_flush = false,
    });
    const auto flush = sink.flush(
        std::chrono::steady_clock::now() + std::chrono::seconds {1});
    const auto closed = sink.close(
        std::chrono::steady_clock::now() + std::chrono::seconds {1});
    const std::string after_close = "after-close\n";
    const auto direct_write = ::write(
        STDERR_FILENO, after_close.data(), after_close.size());

    assert(::dup2(saved_stderr, STDERR_FILENO) == STDERR_FILENO);
    assert(::close(saved_stderr) == 0);
    assert(::close(pipe_fds[1]) == 0);
    std::string captured;
    char buffer[128];
    while (true) {
        const auto received = ::read(pipe_fds[0], buffer, sizeof(buffer));
        if (received == 0)
            break;
        assert(received > 0);
        captured.append(buffer, static_cast<std::size_t>(received));
    }
    assert(::close(pipe_fds[0]) == 0);

    assert(submit == ssv::SsvLogSubmitResult::Accepted);
    assert(flush.status == ssv::SsvLogIoStatus::Completed);
    assert(closed.status == ssv::SsvLogIoStatus::Completed);
    assert(closed.stats.written_records == 1);
    assert(closed.stats.write_failed_records == 0);
    assert(direct_write == static_cast<ssize_t>(after_close.size()));
    assert(captured == "event=test value=first\nafter-close\n");
}

void test_submit_retries_eintr_and_completes_short_write()
{
    const std::string record = "event=test value=retry-short-write\n";
    write_script = {};
    write_script.enabled = true;

    ssv::SsvStderrLogSink sink;
    const auto submit = sink.submit({
        .bytes = record,
        .sequence = 1,
        .emitted_at = std::chrono::system_clock::now(),
        .severity = ssv::SsvEventSeverity::Info,
        .force_flush = false,
    });
    const auto closed = sink.close(
        std::chrono::steady_clock::now() + std::chrono::seconds {1});
    write_script.enabled = false;

    assert(submit == ssv::SsvLogSubmitResult::Accepted);
    assert(closed.stats.written_records == 1);
    assert(closed.stats.write_failed_records == 0);
    assert(write_script.calls == 3);
    assert(write_script.captured == record);
}

} // namespace

int main()
{
    test_submit_writes_complete_record_and_close_keeps_stderr_open();
    test_submit_retries_eintr_and_completes_short_write();
    return 0;
}
