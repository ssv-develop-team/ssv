#pragma once

#include "overlay_presentation.hpp"
#include "overlay_renderer.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

class OverlayRuntime {
public:
    OverlayRuntime(
        std::shared_ptr<SsvSourceMeta> meta,
        bool motion_prediction,
        std::uint32_t max_horizon_ms,
        std::string font_face = "regular",
        std::uint32_t font_size = 7);

    SsvTimelineUpdate on_segment(const SsvTimelineSegment &segment);
    SsvTimelineUpdate on_flush_stop(bool reset_time);
    SsvFrameTiming on_buffer(
        GstClockTime pts,
        GstClockTime duration,
        bool discontinuity);
    SsvTimelineUpdate stop();

    SsvOverlayFrame frame_for_display(const SsvFrameTiming &display_timing);
    OverlayRenderStats render(
        GstVideoFrame &video_frame,
        const SsvFrameTiming &display_timing);

    OverlayPresentationStats stats() const;
    SsvMetaStats meta_stats() const { return presentation_.meta_stats(); }
    bool should_log_summary();

private:
    void reset_display_state();

    std::shared_ptr<SsvSourceMeta> meta_;
    SsvTimelineCursor timeline_;
    OverlayPresentationModel presentation_;
    OverlayRenderer renderer_;
    SsvOverlayFrame reusable_overlay_frame_;
    std::uint64_t renderer_invalid_boxes_ = 0;
    std::chrono::steady_clock::time_point last_summary_at_;
};
