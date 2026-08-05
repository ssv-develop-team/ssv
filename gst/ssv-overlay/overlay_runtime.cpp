#include "overlay_runtime.hpp"

#include <stdexcept>
#include <utility>

OverlayRuntime::OverlayRuntime(
    std::shared_ptr<SsvSourceMeta> meta,
    bool motion_prediction,
    std::uint32_t max_horizon_ms,
    std::string font_face,
    std::uint32_t font_size)
    : meta_(std::move(meta)),
      timeline_(meta_),
      presentation_(meta_, motion_prediction, max_horizon_ms),
      renderer_(font_face, font_size),
      last_summary_at_(std::chrono::steady_clock::now())
{
    if (meta_ == nullptr)
        throw std::invalid_argument("overlay runtime requires source metadata");
}

void OverlayRuntime::reset_display_state()
{
    presentation_.reset();
}

SsvTimelineUpdate OverlayRuntime::on_segment(const SsvTimelineSegment &segment)
{
    const auto update = timeline_.on_segment(segment);
    if (update.reset)
        reset_display_state();
    return update;
}

SsvTimelineUpdate OverlayRuntime::on_flush_stop(bool reset_time)
{
    const auto update = timeline_.on_flush_stop(reset_time);
    if (update.reset)
        reset_display_state();
    return update;
}

SsvFrameTiming OverlayRuntime::on_buffer(
    GstClockTime pts,
    GstClockTime duration,
    bool discontinuity)
{
    const auto update = timeline_.on_buffer(pts, discontinuity);
    if (update.reset)
        reset_display_state();
    return {pts, duration, update.generation};
}

SsvTimelineUpdate OverlayRuntime::stop()
{
    const auto update = timeline_.on_lifecycle_reset();
    reset_display_state();
    return update;
}

SsvOverlayFrame OverlayRuntime::frame_for_display(
    const SsvFrameTiming &display_timing)
{
    return presentation_.present(display_timing);
}

OverlayRenderStats OverlayRuntime::render(
    GstVideoFrame &video_frame,
    const SsvFrameTiming &display_timing)
{
    presentation_.present_into(display_timing, reusable_overlay_frame_);
    auto render_stats = renderer_.render(video_frame, reusable_overlay_frame_);
    renderer_invalid_boxes_ += render_stats.skipped_small_boxes;
    return render_stats;
}

OverlayPresentationStats OverlayRuntime::stats() const
{
    auto result = presentation_.stats();
    result.invalid_boxes += renderer_invalid_boxes_;
    return result;
}

bool OverlayRuntime::should_log_summary()
{
    const auto now = std::chrono::steady_clock::now();
    if (now - last_summary_at_ < std::chrono::seconds(5))
        return false;
    last_summary_at_ = now;
    return true;
}
