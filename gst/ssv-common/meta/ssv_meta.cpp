#include "ssv_meta.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace {

constexpr std::size_t kHistoryCapacity = 64;
constexpr GstClockTime kHistoryWindow = 2 * GST_SECOND;

void note_result(SsvMetaStats &stats, SsvMetaResult result)
{
    switch (result) {
    case SsvMetaResult::Published:
        ++stats.published;
        break;
    case SsvMetaResult::Consumed:
        ++stats.consumed;
        break;
    case SsvMetaResult::Empty:
        ++stats.empty;
        break;
    case SsvMetaResult::NoPts:
        ++stats.no_pts;
        break;
    case SsvMetaResult::Occupied:
        ++stats.occupied;
        break;
    case SsvMetaResult::WrongSource:
        ++stats.wrong_source;
        break;
    case SsvMetaResult::WrongGeneration:
        ++stats.wrong_generation;
        break;
    case SsvMetaResult::DuplicatePts:
        ++stats.duplicate_pts;
        break;
    case SsvMetaResult::StalePts:
        ++stats.stale_pts;
        break;
    }
}

SsvMetaResult validate_timing(GstClockTime pts, GstClockTime last_pts)
{
    if (pts == GST_CLOCK_TIME_NONE)
        return SsvMetaResult::NoPts;
    if (last_pts != GST_CLOCK_TIME_NONE) {
        if (pts == last_pts)
            return SsvMetaResult::DuplicatePts;
        if (pts < last_pts)
            return SsvMetaResult::StalePts;
    }
    return SsvMetaResult::Published;
}

bool normalize_detection(SsvDetection &detection)
{
    if (!std::isfinite(detection.confidence) || detection.confidence < 0.0F ||
        detection.confidence > 1.0F) {
        return false;
    }
    if (!std::isfinite(detection.x1) || !std::isfinite(detection.y1) ||
        !std::isfinite(detection.x2) || !std::isfinite(detection.y2)) {
        return false;
    }

    detection.x1 = std::clamp(detection.x1, 0.0F, 1.0F);
    detection.y1 = std::clamp(detection.y1, 0.0F, 1.0F);
    detection.x2 = std::clamp(detection.x2, 0.0F, 1.0F);
    detection.y2 = std::clamp(detection.y2, 0.0F, 1.0F);
    if (detection.x2 <= detection.x1 || detection.y2 <= detection.y1)
        return false;
    if (detection.class_id < -1)
        detection.class_id = -1;
    return true;
}

void normalize_detections(SsvDetectionFrame &frame)
{
    frame.detections.erase(
        std::remove_if(
            frame.detections.begin(), frame.detections.end(),
            [](auto &detection) { return !normalize_detection(detection); }),
        frame.detections.end());
}

bool normalize_tracked_object(SsvTrackedObject &object)
{
    if (!normalize_detection(object.detection))
        return false;
    if (object.track_id < -1)
        object.track_id = -1;
    if (object.track_state < SSV_TRACK_NEW ||
        object.track_state > SSV_TRACK_DEAD) {
        object.track_state = SSV_TRACK_NEW;
    }
    return true;
}

void normalize_tracked_objects(std::vector<SsvTrackedObject> &objects)
{
    objects.erase(
        std::remove_if(
            objects.begin(), objects.end(),
            [](auto &object) { return !normalize_tracked_object(object); }),
        objects.end());
}

struct SsvMetaRegistry {
    std::mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<SsvSourceMeta>> sources;
};

} // namespace

struct SsvSourceMeta::Impl {
    explicit Impl(std::string source) : source_id(std::move(source)) {}

    void advance_generation()
    {
        ++generation;
        last_detection_pts = GST_CLOCK_TIME_NONE;
        detection_slot.reset();
        publication_slot.reset();
        history.clear();
        ++stats.generation_resets;
    }

    mutable std::mutex mutex;
    std::string source_id;
    std::uint64_t generation = 0;
    std::optional<SsvTimelineSegment> segment;
    GstClockTime last_detection_pts = GST_CLOCK_TIME_NONE;
    std::optional<SsvDetectionFrame> detection_slot;
    std::shared_ptr<const SsvTrackedFrame> publication_slot;
    std::deque<std::shared_ptr<const SsvTrackedFrame>> history;
    SsvMetaStats stats;
};

SsvSourceMeta::SsvSourceMeta(std::string_view source_id)
    : impl_(std::make_unique<Impl>(std::string(source_id)))
{
    if (source_id.empty())
        throw std::invalid_argument("source_id must not be empty");
}

SsvSourceMeta::~SsvSourceMeta() = default;

std::string_view SsvSourceMeta::source_id() const noexcept
{
    return impl_->source_id;
}

std::uint64_t SsvSourceMeta::generation() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->generation;
}

SsvMetaStats SsvSourceMeta::stats() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->stats;
}

SsvMetaResult SsvSourceMeta::publish_detection(SsvDetectionFrame &&frame)
{
    normalize_detections(frame);
    std::lock_guard<std::mutex> lock(impl_->mutex);

    SsvMetaResult result = SsvMetaResult::Published;
    if (frame.source_id != impl_->source_id)
        result = SsvMetaResult::WrongSource;
    else if (frame.timing.generation != impl_->generation)
        result = SsvMetaResult::WrongGeneration;
    else
        result = validate_timing(frame.timing.pts, impl_->last_detection_pts);
    if (result == SsvMetaResult::Published && impl_->detection_slot)
        result = SsvMetaResult::Occupied;
    if (result != SsvMetaResult::Published) {
        frame.analysis_frame.reset();
        note_result(impl_->stats, result);
        return result;
    }

    impl_->last_detection_pts = frame.timing.pts;
    impl_->detection_slot.emplace(std::move(frame));
    note_result(impl_->stats, SsvMetaResult::Published);
    return SsvMetaResult::Published;
}

SsvDetectionConsumeResult SsvSourceMeta::consume_detection()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->detection_slot) {
        note_result(impl_->stats, SsvMetaResult::Empty);
        return {};
    }

    SsvDetectionConsumeResult consumed;
    consumed.result = SsvMetaResult::Consumed;
    consumed.frame.emplace(std::move(*impl_->detection_slot));
    impl_->detection_slot.reset();
    note_result(impl_->stats, SsvMetaResult::Consumed);
    return consumed;
}

SsvMetaResult SsvSourceMeta::publish_tracked(
    SsvDetectionFrame &&observation,
    std::vector<SsvTrackedObject> objects)
{
    // Pixel ownership ends at the tracked snapshot boundary on every outcome.
    observation.analysis_frame.reset();
    normalize_tracked_objects(objects);
    std::lock_guard<std::mutex> lock(impl_->mutex);

    SsvMetaResult result = SsvMetaResult::Published;
    if (observation.source_id != impl_->source_id)
        result = SsvMetaResult::WrongSource;
    else if (observation.timing.generation != impl_->generation)
        result = SsvMetaResult::WrongGeneration;
    else {
        const auto last_pts = impl_->history.empty()
            ? GST_CLOCK_TIME_NONE
            : impl_->history.back()->timing.pts;
        result = validate_timing(observation.timing.pts, last_pts);
    }
    if (result == SsvMetaResult::Published && impl_->publication_slot)
        result = SsvMetaResult::Occupied;
    if (result != SsvMetaResult::Published) {
        note_result(impl_->stats, result);
        return result;
    }

    SsvTrackedFrame frame;
    frame.frame_id = observation.frame_id;
    frame.source_id = std::move(observation.source_id);
    frame.timing = observation.timing;
    frame.objects = std::move(objects);
    auto snapshot = std::make_shared<const SsvTrackedFrame>(std::move(frame));
    impl_->publication_slot = snapshot;
    impl_->history.push_back(std::move(snapshot));

    const auto newest_pts = impl_->history.back()->timing.pts;
    while (!impl_->history.empty() &&
           newest_pts - impl_->history.front()->timing.pts > kHistoryWindow) {
        impl_->history.pop_front();
    }
    while (impl_->history.size() > kHistoryCapacity)
        impl_->history.pop_front();
    impl_->stats.max_history_depth = std::max(
        impl_->stats.max_history_depth, impl_->history.size());
    note_result(impl_->stats, SsvMetaResult::Published);
    return SsvMetaResult::Published;
}

SsvTrackedConsumeResult SsvSourceMeta::consume_tracked()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->publication_slot) {
        note_result(impl_->stats, SsvMetaResult::Empty);
        return {};
    }

    auto snapshot = std::move(impl_->publication_slot);
    impl_->publication_slot.reset();
    note_result(impl_->stats, SsvMetaResult::Consumed);
    return {SsvMetaResult::Consumed, std::move(snapshot)};
}

std::shared_ptr<const SsvTrackedFrame>
SsvSourceMeta::latest_tracked_at_or_before(GstClockTime display_pts) const
{
    if (display_pts == GST_CLOCK_TIME_NONE)
        return nullptr;

    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto match = std::upper_bound(
        impl_->history.begin(), impl_->history.end(), display_pts,
        [](GstClockTime pts, const auto &snapshot) {
            return pts < snapshot->timing.pts;
        });
    return match == impl_->history.begin() ? nullptr : *std::prev(match);
}

std::size_t SsvSourceMeta::history_depth() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->history.size();
}

std::uint64_t SsvSourceMeta::observe_segment(
    const SsvTimelineSegment &segment,
    std::uint64_t expected_generation,
    bool coalesce_reset)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->segment || *impl_->segment != segment) {
        if (!coalesce_reset && impl_->generation == expected_generation)
            impl_->advance_generation();
        impl_->segment = segment;
    }
    return impl_->generation;
}

std::uint64_t SsvSourceMeta::request_reset(
    std::uint64_t expected_generation)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->generation == expected_generation)
        impl_->advance_generation();
    return impl_->generation;
}

SsvTimelineCursor::SsvTimelineCursor(std::shared_ptr<SsvSourceMeta> source)
    : source_(std::move(source))
{
    if (!source_)
        throw std::invalid_argument("source meta must not be null");
}

SsvTimelineUpdate SsvTimelineCursor::synchronize()
{
    const auto current = source_->generation();
    const bool changed = current != generation_;
    if (changed) {
        generation_ = current;
        last_pts_ = GST_CLOCK_TIME_NONE;
    }
    return {generation_, changed};
}

SsvTimelineUpdate SsvTimelineCursor::reset_once(ResetKind kind)
{
    if (last_reset_kind_ != kind)
        generation_ = source_->request_reset(generation_);
    else
        generation_ = source_->generation();
    last_pts_ = GST_CLOCK_TIME_NONE;
    last_reset_kind_ = kind;
    return {generation_, true};
}

SsvTimelineUpdate SsvTimelineCursor::on_segment(
    const SsvTimelineSegment &segment)
{
    const auto previous = generation_;
    generation_ = source_->observe_segment(
        segment, generation_, last_reset_kind_ != ResetKind::None);
    last_pts_ = GST_CLOCK_TIME_NONE;
    last_reset_kind_ = ResetKind::None;
    return {generation_, generation_ != previous};
}

SsvTimelineUpdate SsvTimelineCursor::on_flush_stop(bool reset_time)
{
    const auto synchronized = synchronize();
    if (!reset_time)
        return synchronized;
    if (synchronized.reset) {
        last_reset_kind_ = ResetKind::Flush;
        return synchronized;
    }
    return reset_once(ResetKind::Flush);
}

SsvTimelineUpdate SsvTimelineCursor::on_buffer(
    GstClockTime pts,
    bool discontinuity)
{
    const auto synchronized = synchronize();
    if (synchronized.reset) {
        last_reset_kind_ = discontinuity
            ? ResetKind::Discontinuity
            : ResetKind::None;
        if (pts != GST_CLOCK_TIME_NONE)
            last_pts_ = pts;
        return synchronized;
    }

    const bool rollback = pts != GST_CLOCK_TIME_NONE &&
        last_pts_ != GST_CLOCK_TIME_NONE && pts < last_pts_;
    if (discontinuity || rollback) {
        auto update = reset_once(
            discontinuity ? ResetKind::Discontinuity : ResetKind::PtsRollback);
        if (pts != GST_CLOCK_TIME_NONE)
            last_pts_ = pts;
        return update;
    }

    last_reset_kind_ = ResetKind::None;
    if (pts != GST_CLOCK_TIME_NONE)
        last_pts_ = pts;
    return {generation_, false};
}

SsvTimelineUpdate SsvTimelineCursor::on_lifecycle_reset()
{
    const auto synchronized = synchronize();
    if (synchronized.reset) {
        last_reset_kind_ = ResetKind::Lifecycle;
        return synchronized;
    }
    return reset_once(ResetKind::Lifecycle);
}

SsvSourceContext::SsvSourceContext(std::string_view source_id)
    : meta_(ssv_meta(source_id))
{
}

std::shared_ptr<SsvSourceMeta> SsvSourceContext::meta() const
{
    return meta_;
}

std::string_view SsvSourceContext::source_id() const noexcept
{
    return meta_->source_id();
}

std::shared_ptr<SsvSourceMeta> ssv_meta(std::string_view source_id)
{
    if (source_id.empty())
        throw std::invalid_argument("source_id must not be empty");

    static SsvMetaRegistry registry;
    std::lock_guard<std::mutex> lock(registry.mutex);
    const std::string key(source_id);
    auto [entry, inserted] = registry.sources.try_emplace(key);
    if (inserted)
        entry->second = std::make_shared<SsvSourceMeta>(key);
    return entry->second;
}
