#include "core/ssv_latest_frame_scheduler.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ssv::infer {
namespace {

using Clock = std::chrono::steady_clock;

} // namespace

class SsvLatestFrameScheduler::Impl {
private:
    enum class TaskState {
        Pending,
        Running,
        Completed,
        Replaced,
        Cancelled,
        Failed,
    };

    struct Task {
        explicit Task(SsvInferenceRequest value)
            : request(std::move(value))
            , queued_at(Clock::now())
        {
        }

        SsvInferenceRequest request;
        TaskState state = TaskState::Pending;
        std::stop_source stop_source;
        SsvDetectionFrame detections;
        SsvInferenceStageTimings timings;
        std::string error;
        Clock::time_point queued_at;
    };

public:
    explicit Impl(Execute execute)
        : execute_(std::move(execute))
    {
        if (!execute_)
            throw std::invalid_argument("inference scheduler requires an executor");
    }

    ~Impl() { stop(); }

    void start()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (started_ || running_ || worker_.joinable())
            throw std::logic_error("inference scheduler has already started");

        stats_epoch_ = Clock::now();
        accepting_ = true;
        running_ = true;
        stopping_ = false;
        try {
            worker_ = std::thread(&Impl::worker_loop, this);
            started_ = true;
        } catch (...) {
            accepting_ = false;
            running_ = false;
            stopping_ = true;
            throw;
        }
    }

    SsvInferenceSubmissionResult submit(SsvInferenceRequest request)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!accepting_) {
            return {
                SsvInferenceSubmissionStatus::Cancelled,
                {},
                "inference service is stopped",
            };
        }

        auto task = std::make_shared<Task>(std::move(request));
        ++stats_.submitted;
        ++stats_window_received_;
        if (pending_) {
            pending_->state = TaskState::Replaced;
            ++stats_.replaced;
            ++stats_window_dropped_;
        }
        pending_ = task;
        stats_.pending = 1;
        stats_.max_pending = std::max(stats_.max_pending, stats_.pending);
        condition_.notify_all();

        condition_.wait(lock, [&task] { return terminal(task->state); });
        return {
            public_status(task->state),
            std::move(task->detections),
            std::move(task->error),
        };
    }

    void cancel(std::string_view source_id) noexcept
    {
        std::optional<std::stop_source> in_flight_stop;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pending_ && pending_->request.source_id == source_id) {
                pending_->state = TaskState::Cancelled;
                pending_.reset();
                stats_.pending = 0;
                ++stats_.cancelled;
                ++stats_window_dropped_;
            }
            if (in_flight_ && in_flight_->request.source_id == source_id)
                in_flight_stop = in_flight_->stop_source;
            condition_.notify_all();
        }
        if (in_flight_stop)
            in_flight_stop->request_stop();
    }

    void stop() noexcept
    {
        std::optional<std::stop_source> in_flight_stop;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            accepting_ = false;
            stopping_ = true;
            running_ = false;
            if (pending_) {
                pending_->state = TaskState::Cancelled;
                pending_.reset();
                stats_.pending = 0;
                ++stats_.cancelled;
                ++stats_window_dropped_;
            }
            if (in_flight_)
                in_flight_stop = in_flight_->stop_source;
            condition_.notify_all();
        }
        if (in_flight_stop)
            in_flight_stop->request_stop();
        if (worker_.joinable())
            worker_.join();
    }

    bool running() const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return running_;
    }

    SsvInferenceStats stats() const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return stats_;
    }

    SsvInferenceStatsWindow take_stats_window()
    {
        SsvInferenceStatsWindowInput input;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto ended_at_us = now_us();
            input.started_at_us = stats_window_started_us_;
            input.ended_at_us = ended_at_us;
            input.previous_completion_us =
                stats_window_previous_completion_us_;
            input.received = stats_window_received_;
            input.dropped = stats_window_dropped_;
            input.completed_samples = std::move(stats_window_samples_);
            stats_window_started_us_ = ended_at_us;
            stats_window_previous_completion_us_ = last_completion_us_;
            stats_window_received_ = 0;
            stats_window_dropped_ = 0;
            stats_window_samples_.clear();
        }
        return ssv_inference_stats_summarize(input);
    }

private:
    static bool terminal(TaskState state) noexcept
    {
        return state == TaskState::Completed
            || state == TaskState::Replaced
            || state == TaskState::Cancelled
            || state == TaskState::Failed;
    }

    static SsvInferenceSubmissionStatus public_status(TaskState state)
    {
        switch (state) {
        case TaskState::Completed:
            return SsvInferenceSubmissionStatus::Completed;
        case TaskState::Replaced:
            return SsvInferenceSubmissionStatus::Replaced;
        case TaskState::Cancelled:
            return SsvInferenceSubmissionStatus::Cancelled;
        case TaskState::Failed:
            return SsvInferenceSubmissionStatus::Failed;
        case TaskState::Pending:
        case TaskState::Running:
            break;
        }
        throw std::logic_error("inference task has not reached a terminal state");
    }

    std::uint64_t now_us() const noexcept
    {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                Clock::now() - stats_epoch_)
                .count());
    }

    void worker_loop() noexcept
    {
        while (true) {
            std::shared_ptr<Task> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [this] {
                    return stopping_ || pending_ != nullptr;
                });
                if (stopping_ && !pending_)
                    return;
                task = std::move(pending_);
                stats_.pending = 0;
                task->state = TaskState::Running;
                in_flight_ = task;
                stats_.in_flight = 1;
                task->timings.queue_us = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        Clock::now() - task->queued_at)
                        .count());
                stats_.max_in_flight =
                    std::max(stats_.max_in_flight, stats_.in_flight);
            }

            try {
                auto run = execute_(task->request, task->stop_source.get_token());
                task->detections = std::move(run.detections);
                run.timings.queue_us = task->timings.queue_us;
                if (run.timings.total_us && run.timings.queue_us)
                    *run.timings.total_us += *run.timings.queue_us;
                task->timings = run.timings;
            } catch (const std::exception &error) {
                task->error = error.what();
            } catch (...) {
                task->error = "unknown inference failure";
            }

            // The executor no longer needs the input after it returns. Drop
            // the worker's frame reference before publishing a terminal
            // state so failed and cancelled submissions observe released
            // staging leases when they wake up.
            task->request.analysis_frame.reset();

            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (task->stop_source.stop_requested() || stopping_) {
                    task->state = TaskState::Cancelled;
                    ++stats_.cancelled;
                    ++stats_window_dropped_;
                } else if (!task->error.empty()) {
                    task->state = TaskState::Failed;
                    ++stats_.failed;
                } else {
                    task->state = TaskState::Completed;
                    ++stats_.completed;
                    const auto completed_at_us = now_us();
                    last_completion_us_ = completed_at_us;
                    stats_window_samples_.push_back({
                        completed_at_us,
                        task->timings,
                    });
                }
                in_flight_.reset();
                stats_.in_flight = 0;
                condition_.notify_all();
            }
        }
    }

    Execute execute_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::thread worker_;
    std::shared_ptr<Task> in_flight_;
    std::shared_ptr<Task> pending_;
    SsvInferenceStats stats_;
    Clock::time_point stats_epoch_;
    std::uint64_t stats_window_started_us_ = 0;
    std::optional<std::uint64_t> stats_window_previous_completion_us_;
    std::optional<std::uint64_t> last_completion_us_;
    std::uint64_t stats_window_received_ = 0;
    std::uint64_t stats_window_dropped_ = 0;
    std::vector<SsvInferenceTimingSample> stats_window_samples_;
    bool accepting_ = false;
    bool running_ = false;
    bool stopping_ = false;
    bool started_ = false;
};

SsvLatestFrameScheduler::SsvLatestFrameScheduler(Execute execute)
    : impl_(std::make_unique<Impl>(std::move(execute)))
{
}

SsvLatestFrameScheduler::~SsvLatestFrameScheduler() = default;

void SsvLatestFrameScheduler::start()
{
    impl_->start();
}

SsvInferenceSubmissionResult SsvLatestFrameScheduler::submit(
    SsvInferenceRequest request)
{
    return impl_->submit(std::move(request));
}

void SsvLatestFrameScheduler::cancel(std::string_view source_id) noexcept
{
    impl_->cancel(source_id);
}

void SsvLatestFrameScheduler::stop() noexcept
{
    impl_->stop();
}

bool SsvLatestFrameScheduler::running() const noexcept
{
    return impl_->running();
}

SsvInferenceStats SsvLatestFrameScheduler::stats() const noexcept
{
    return impl_->stats();
}

SsvInferenceStatsWindow SsvLatestFrameScheduler::take_stats_window()
{
    return impl_->take_stats_window();
}

} // namespace ssv::infer
