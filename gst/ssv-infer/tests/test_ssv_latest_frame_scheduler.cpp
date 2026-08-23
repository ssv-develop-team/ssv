#include "core/ssv_latest_frame_scheduler.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {

using namespace std::chrono_literals;

class ControlledExecutor final {
public:
    ssv::infer::SsvInferenceRunResult operator()(
        const ssv::infer::SsvInferenceRequest &request,
        std::stop_token stop_token)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        ++entered_count_;
        const auto call_index = entered_count_;
        ++active_count_;
        max_active_count_ = std::max(max_active_count_, active_count_);
        condition_.notify_all();

        std::stop_callback wake_on_stop(
            stop_token, [this] { condition_.notify_all(); });
        condition_.wait(lock, [this, &stop_token] {
            return completion_permits_ > 0
                || stop_token.stop_requested();
        });
        const bool cancelled = stop_token.stop_requested();
        if (!cancelled)
            --completion_permits_;
        const bool should_fail = failure_call_ == call_index;
        --active_count_;
        lock.unlock();

        if (cancelled)
            throw std::runtime_error("controlled scheduler execution cancelled");
        if (should_fail)
            throw std::runtime_error("controlled scheduler execution failure");

        ssv::infer::SsvInferenceRunResult result;
        result.detections.frame_id = request.frame_id;
        result.detections.source_id = request.source_id;
        result.timings.backend_execution_us = 100;
        result.timings.total_us = 100;
        return result;
    }

    bool wait_until_entered(std::size_t expected)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, 2s,
            [this, expected] { return entered_count_ >= expected; });
    }

    void allow_one_completion()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++completion_permits_;
        condition_.notify_all();
    }

    void fail_on_call(std::size_t call_index)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        failure_call_ = call_index;
    }

    std::size_t max_active_count() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return max_active_count_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::size_t entered_count_ = 0;
    std::size_t active_count_ = 0;
    std::size_t max_active_count_ = 0;
    std::size_t completion_permits_ = 0;
    std::size_t failure_call_ = 0;
};

ssv::infer::SsvInferenceRequest make_request(
    std::uint64_t frame_id,
    std::string source_id = "camera-01")
{
    ssv::infer::SsvInferenceRequest request;
    request.frame_id = frame_id;
    request.source_id = std::move(source_id);
    return request;
}

bool wait_for_pending(
    const ssv::infer::SsvLatestFrameScheduler &scheduler,
    std::uint64_t expected)
{
    const auto deadline =
        std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (scheduler.stats().pending == expected)
            return true;
        std::this_thread::yield();
    }
    return false;
}

void test_scheduler_replaces_only_latest_pending()
{
    auto executor = std::make_shared<ControlledExecutor>();
    ssv::infer::SsvLatestFrameScheduler scheduler(
        [executor](const auto &request, std::stop_token stop_token) {
            return (*executor)(request, stop_token);
        });
    scheduler.start();

    auto first = std::async(std::launch::async, [&scheduler] {
        return scheduler.submit(make_request(1));
    });
    assert(executor->wait_until_entered(1));

    auto second = std::async(std::launch::async, [&scheduler] {
        return scheduler.submit(make_request(2));
    });
    assert(wait_for_pending(scheduler, 1));

    auto third = std::async(std::launch::async, [&scheduler] {
        return scheduler.submit(make_request(3));
    });
    assert(second.wait_for(2s) == std::future_status::ready);
    assert(second.get().status
        == ssv::infer::SsvInferenceSubmissionStatus::Replaced);

    executor->allow_one_completion();
    assert(first.get().status
        == ssv::infer::SsvInferenceSubmissionStatus::Completed);
    assert(executor->wait_until_entered(2));
    executor->allow_one_completion();
    const auto third_result = third.get();
    assert(third_result.status
        == ssv::infer::SsvInferenceSubmissionStatus::Completed);
    assert(third_result.detections.frame_id == 3);

    const auto stats = scheduler.stats();
    assert(stats.submitted == 3);
    assert(stats.completed == 2);
    assert(stats.replaced == 1);
    assert(stats.max_in_flight == 1);
    assert(stats.max_pending == 1);
    assert(executor->max_active_count() == 1);
}

void test_scheduler_cancel_and_stop_release_submitters()
{
    auto executor = std::make_shared<ControlledExecutor>();
    ssv::infer::SsvLatestFrameScheduler scheduler(
        [executor](const auto &request, std::stop_token stop_token) {
            return (*executor)(request, stop_token);
        });
    scheduler.start();

    auto first = std::async(std::launch::async, [&scheduler] {
        return scheduler.submit(make_request(1, "cancelled-source"));
    });
    assert(executor->wait_until_entered(1));
    auto second = std::async(std::launch::async, [&scheduler] {
        return scheduler.submit(make_request(2, "cancelled-source"));
    });
    assert(wait_for_pending(scheduler, 1));

    scheduler.cancel("cancelled-source");
    assert(first.wait_for(2s) == std::future_status::ready);
    assert(second.wait_for(2s) == std::future_status::ready);
    assert(first.get().status
        == ssv::infer::SsvInferenceSubmissionStatus::Cancelled);
    assert(second.get().status
        == ssv::infer::SsvInferenceSubmissionStatus::Cancelled);
    assert(scheduler.running());

    auto third = std::async(std::launch::async, [&scheduler] {
        return scheduler.submit(make_request(3));
    });
    assert(executor->wait_until_entered(2));
    auto fourth = std::async(std::launch::async, [&scheduler] {
        return scheduler.submit(make_request(4));
    });
    assert(wait_for_pending(scheduler, 1));

    scheduler.stop();
    assert(third.wait_for(2s) == std::future_status::ready);
    assert(fourth.wait_for(2s) == std::future_status::ready);
    assert(third.get().status
        == ssv::infer::SsvInferenceSubmissionStatus::Cancelled);
    assert(fourth.get().status
        == ssv::infer::SsvInferenceSubmissionStatus::Cancelled);
    assert(!scheduler.running());

    const auto stopped = scheduler.submit(make_request(5));
    assert(stopped.status
        == ssv::infer::SsvInferenceSubmissionStatus::Cancelled);
}

void test_scheduler_failure_does_not_stop_worker()
{
    auto executor = std::make_shared<ControlledExecutor>();
    executor->fail_on_call(1);
    ssv::infer::SsvLatestFrameScheduler scheduler(
        [executor](const auto &request, std::stop_token stop_token) {
            return (*executor)(request, stop_token);
        });
    scheduler.start();

    auto first = std::async(std::launch::async, [&scheduler] {
        return scheduler.submit(make_request(1));
    });
    assert(executor->wait_until_entered(1));
    executor->allow_one_completion();
    const auto first_result = first.get();
    assert(first_result.status
        == ssv::infer::SsvInferenceSubmissionStatus::Failed);
    assert(first_result.error == "controlled scheduler execution failure");
    assert(scheduler.running());

    auto second = std::async(std::launch::async, [&scheduler] {
        return scheduler.submit(make_request(2));
    });
    assert(executor->wait_until_entered(2));
    executor->allow_one_completion();
    const auto second_result = second.get();
    assert(second_result.status
        == ssv::infer::SsvInferenceSubmissionStatus::Completed);
}

void test_scheduler_reports_queue_latency_and_window_stats()
{
    auto executor = std::make_shared<ControlledExecutor>();
    ssv::infer::SsvLatestFrameScheduler scheduler(
        [executor](const auto &request, std::stop_token stop_token) {
            return (*executor)(request, stop_token);
        });
    scheduler.start();

    auto first = std::async(std::launch::async, [&scheduler] {
        return scheduler.submit(make_request(1));
    });
    assert(executor->wait_until_entered(1));
    auto second = std::async(std::launch::async, [&scheduler] {
        return scheduler.submit(make_request(2));
    });
    assert(wait_for_pending(scheduler, 1));
    std::this_thread::sleep_for(10ms);

    executor->allow_one_completion();
    assert(first.get().status
        == ssv::infer::SsvInferenceSubmissionStatus::Completed);
    assert(executor->wait_until_entered(2));
    executor->allow_one_completion();
    assert(second.get().status
        == ssv::infer::SsvInferenceSubmissionStatus::Completed);

    const auto window = scheduler.take_stats_window();
    assert(window.received == 2);
    assert(window.dropped == 0);
    assert(window.completed == 2);
    assert(window.queue.p50_us > 0);
    assert(window.backend_execution.p50_us
        && *window.backend_execution.p50_us == 100);
    assert(window.total.p50_us
        && window.queue.p50_us
        && *window.total.p50_us >= *window.queue.p50_us + 100);

    const auto empty = scheduler.take_stats_window();
    assert(empty.received == 0);
    assert(empty.completed == 0);
}

} // namespace

int main()
{
    test_scheduler_replaces_only_latest_pending();
    test_scheduler_cancel_and_stop_release_submitters();
    test_scheduler_failure_does_not_stop_worker();
    test_scheduler_reports_queue_latency_and_window_stats();
    return 0;
}
