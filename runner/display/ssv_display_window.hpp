#pragma once

#include "ssv_config.hpp"
#include "ssv_meta.hpp"
#include "pipeline/ssv_pipeline_plan.hpp"
#include "ssv_window_lifecycle.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ssv {

class SsvDisplayAttachment;

struct SsvDisplayWindowSpec {
    std::string source_id;
    SsvGlBackend gl_backend = SsvGlBackend::Auto;
    SsvOverlayConfig overlay;
    SsvResolvedDisplayBackend backend =
        SsvResolvedDisplayBackend::GtkGlSink;
    std::shared_ptr<SsvSourceContext> source_context;
};

struct SsvDisplayViewport {
    int width = 0;
    int height = 0;
    int scale_factor = 1;
};

struct SsvVideoSize {
    int width = 0;
    int height = 0;
    int pixel_aspect_ratio_numerator = 1;
    int pixel_aspect_ratio_denominator = 1;
};

struct SsvDisplayRect {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

[[nodiscard]] std::optional<SsvDisplayRect> ssv_display_content_rect(
    const SsvDisplayViewport &viewport,
    const SsvVideoSize &video);

[[nodiscard]] std::optional<SsvDisplayRect> ssv_display_map_bbox(
    const SsvDisplayRect &content,
    const SsvDetection &detection);

[[nodiscard]] std::string ssv_display_overlay_label(
    const SsvOverlayBox &box);

[[nodiscard]] SsvGlBackend ssv_display_resolve_auto_gl_backend(
    std::string_view session_type,
    bool has_x11_display,
    bool has_wayland_display);

class SsvDisplayWindowError final : public std::runtime_error {
public:
    SsvDisplayWindowError(std::string stage, std::string message);

    [[nodiscard]] const std::string &stage() const noexcept;

private:
    std::string stage_;
};

class SsvDisplayWindow final : public SsvWindowLifecycle {
public:
    /// All operations, including creation and destruction, must run on the
    /// thread that owns the default GLib context and GTK main loop.
    static void initialize(SsvGlBackend gl_backend);

    /// Borrows the attachment's retained sink and timing-pad handles through
    /// close(). Attachment ownership may move, but its final owner must retain
    /// those handles until then.
    [[nodiscard]] static std::unique_ptr<SsvDisplayWindow> create(
        SsvDisplayWindowSpec spec,
        const SsvDisplayAttachment *attachment);

    ~SsvDisplayWindow() override;

    SsvDisplayWindow(const SsvDisplayWindow &) = delete;
    SsvDisplayWindow &operator=(const SsvDisplayWindow &) = delete;

    void show(std::function<void()> on_close) override;
    void close() noexcept override;

private:
    class Impl;
    explicit SsvDisplayWindow(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

} // namespace ssv
