#include "ssv_display_window.hpp"
#include "overlay_presentation.hpp"
#include "pipeline/ssv_pipeline_instance.hpp"

#include <gtk/gtk.h>
#include <gst/video/video.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace ssv {
namespace {

double align_to_device_pixel(double logical_value, int scale_factor)
{
    return std::round(logical_value * scale_factor) / scale_factor;
}

} // namespace

std::optional<SsvDisplayRect> ssv_display_content_rect(
    const SsvDisplayViewport &viewport,
    const SsvVideoSize &video)
{
    if (viewport.width <= 0 || viewport.height <= 0
        || viewport.scale_factor <= 0
        || video.width <= 0 || video.height <= 0
        || video.pixel_aspect_ratio_numerator <= 0
        || video.pixel_aspect_ratio_denominator <= 0) {
        return std::nullopt;
    }

    const double viewport_width = viewport.width;
    const double viewport_height = viewport.height;
    const double video_aspect =
        static_cast<double>(video.width)
        * video.pixel_aspect_ratio_numerator
        / (static_cast<double>(video.height)
            * video.pixel_aspect_ratio_denominator);
    const double viewport_aspect = viewport_width / viewport_height;

    double width = viewport_width;
    double height = viewport_height;
    if (viewport_aspect > video_aspect)
        width = height * video_aspect;
    else
        height = width / video_aspect;

    width = align_to_device_pixel(width, viewport.scale_factor);
    height = align_to_device_pixel(height, viewport.scale_factor);
    return SsvDisplayRect {
        align_to_device_pixel(
            (viewport_width - width) / 2.0, viewport.scale_factor),
        align_to_device_pixel(
            (viewport_height - height) / 2.0, viewport.scale_factor),
        width,
        height,
    };
}

std::optional<SsvDisplayRect> ssv_display_map_bbox(
    const SsvDisplayRect &content,
    const SsvDetection &detection)
{
    if (content.width <= 0.0 || content.height <= 0.0
        || !std::isfinite(detection.x1) || !std::isfinite(detection.y1)
        || !std::isfinite(detection.x2) || !std::isfinite(detection.y2)) {
        return std::nullopt;
    }
    const double x1 = std::clamp<double>(detection.x1, 0.0, 1.0);
    const double y1 = std::clamp<double>(detection.y1, 0.0, 1.0);
    const double x2 = std::clamp<double>(detection.x2, 0.0, 1.0);
    const double y2 = std::clamp<double>(detection.y2, 0.0, 1.0);
    if (x2 <= x1 || y2 <= y1)
        return std::nullopt;
    return SsvDisplayRect {
        content.x + x1 * content.width,
        content.y + y1 * content.height,
        (x2 - x1) * content.width,
        (y2 - y1) * content.height,
    };
}

std::string ssv_display_overlay_label(const SsvOverlayBox &box)
{
    std::array<char, 128> label {};
    std::snprintf(
        label.data(), label.size(), "%s #%d %.2f",
        box.detection.class_name,
        box.track_id,
        box.detection.confidence);
    return label.data();
}

namespace {

struct DisplayUpdate {
    std::optional<SsvFrameTiming> timing;
    std::optional<SsvVideoSize> video_size;
};

std::shared_ptr<SsvSourceContext> resolve_display_source_context(
    const SsvDisplayWindowSpec &spec)
{
    if (spec.source_context != nullptr) {
        if (spec.source_context->source_id() != spec.source_id) {
            throw SsvDisplayWindowError(
                "display.source-context",
                "display source-context does not match source-id");
        }
        return spec.source_context;
    }
    return std::make_shared<SsvSourceContext>(spec.source_id);
}

class DisplayState final
    : public std::enable_shared_from_this<DisplayState> {
public:
    explicit DisplayState(const SsvDisplayWindowSpec &spec)
        : source_context_(resolve_display_source_context(spec))
        , source_id_(source_context_->source_id())
        , overlay_enabled_(spec.overlay.enabled)
        , font_face_(spec.overlay.font.face)
        , font_size_(spec.overlay.font.size)
        , presentation_(
              source_context_->meta(),
              spec.overlay.motion_prediction.enabled,
              spec.overlay.motion_prediction.max_horizon_ms)
        , main_thread_(std::this_thread::get_id())
    {
    }

    ~DisplayState() { destroy_widgets(); }

    void build_widgets(GtkWidget *video_widget)
    {
        require_main_thread("display.gtk.widget");
        if (!GTK_IS_WIDGET(video_widget)
            || gtk_widget_get_parent(video_widget) != nullptr) {
            throw SsvDisplayWindowError(
                "display.gtk.widget",
                "display sink did not provide an unparented GTK widget");
        }

        window_ = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        if (window_ == nullptr) {
            throw SsvDisplayWindowError(
                "display.gtk.window", "failed to create GtkWindow");
        }
        g_object_ref_sink(window_);
        const std::string title = "Site Safety Vision - " + source_id_;
        gtk_window_set_title(GTK_WINDOW(window_), title.c_str());
        gtk_window_set_default_size(GTK_WINDOW(window_), 1280, 720);

        overlay_ = gtk_overlay_new();
        drawing_area_ = gtk_drawing_area_new();
        if (overlay_ == nullptr || drawing_area_ == nullptr) {
            throw SsvDisplayWindowError(
                "display.gtk.window", "failed to create display widgets");
        }
        gtk_widget_set_hexpand(video_widget, TRUE);
        gtk_widget_set_vexpand(video_widget, TRUE);
        gtk_widget_set_halign(video_widget, GTK_ALIGN_FILL);
        gtk_widget_set_valign(video_widget, GTK_ALIGN_FILL);
        gtk_widget_set_hexpand(drawing_area_, TRUE);
        gtk_widget_set_vexpand(drawing_area_, TRUE);
        gtk_widget_set_halign(drawing_area_, GTK_ALIGN_FILL);
        gtk_widget_set_valign(drawing_area_, GTK_ALIGN_FILL);
        gtk_widget_set_app_paintable(drawing_area_, TRUE);

        if (auto *screen = gtk_widget_get_screen(window_)) {
            if (auto *visual = gdk_screen_get_rgba_visual(screen))
                gtk_widget_set_visual(drawing_area_, visual);
        }

        gtk_container_add(GTK_CONTAINER(window_), overlay_);
        gtk_container_add(GTK_CONTAINER(overlay_), video_widget);
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay_), drawing_area_);
        gtk_overlay_set_overlay_pass_through(
            GTK_OVERLAY(overlay_), drawing_area_, TRUE);
        g_signal_connect(
            drawing_area_, "draw", G_CALLBACK(&DisplayState::on_draw), this);
        g_signal_connect(
            window_, "delete-event",
            G_CALLBACK(&DisplayState::on_delete_event), this);
    }

    void show(std::function<void()> on_close)
    {
        require_main_thread("display.gtk.show");
        if (window_ == nullptr) {
            throw SsvDisplayWindowError(
                "display.gtk.show", "display window is already closed");
        }
        on_close_ = std::move(on_close);
        gtk_widget_show_all(window_);
    }

    void post(DisplayUpdate update)
    {
        bool schedule = false;
        {
            std::lock_guard<std::mutex> lock(mailbox_mutex_);
            if (closed_)
                return;
            if (update.timing)
                pending_.timing = update.timing;
            if (update.video_size)
                pending_.video_size = update.video_size;
            if (!dispatch_pending_) {
                dispatch_pending_ = true;
                schedule = true;
            }
        }
        if (!schedule)
            return;
        auto *lease = new std::shared_ptr<DisplayState>(shared_from_this());
        g_main_context_invoke_full(
            g_main_context_default(),
            G_PRIORITY_DEFAULT,
            &DisplayState::dispatch_update,
            lease,
            [](gpointer data) {
                delete static_cast<std::shared_ptr<DisplayState> *>(data);
            });
    }

    void close() noexcept
    {
        {
            std::lock_guard<std::mutex> lock(mailbox_mutex_);
            closed_ = true;
            pending_ = {};
        }
        destroy_widgets();
    }

    [[nodiscard]] std::shared_ptr<SsvSourceContext> source_context()
        const noexcept
    {
        return source_context_;
    }

private:
    void require_main_thread(const char *stage) const
    {
        if (std::this_thread::get_id() != main_thread_) {
            throw SsvDisplayWindowError(
                stage, "GTK operation was requested from a non-main thread");
        }
    }

    static gboolean dispatch_update(gpointer data)
    {
        auto state = *static_cast<std::shared_ptr<DisplayState> *>(data);
        DisplayUpdate update;
        {
            std::lock_guard<std::mutex> lock(state->mailbox_mutex_);
            if (state->closed_) {
                state->dispatch_pending_ = false;
                state->pending_ = {};
                return G_SOURCE_REMOVE;
            }
            update = std::move(state->pending_);
            state->pending_ = {};
            state->dispatch_pending_ = false;
        }
        if (update.timing)
            state->display_timing_ = *update.timing;
        if (update.video_size)
            state->video_size_ = *update.video_size;
        if (state->drawing_area_ != nullptr)
            gtk_widget_queue_draw(state->drawing_area_);
        return G_SOURCE_REMOVE;
    }

    static gboolean on_delete_event(
        GtkWidget *, GdkEvent *, gpointer user_data)
    {
        auto &state = *static_cast<DisplayState *>(user_data);
        if (!state.closing_ && state.on_close_) {
            auto on_close = state.on_close_;
            on_close();
        }
        return TRUE;
    }

    static gboolean on_draw(
        GtkWidget *widget, cairo_t *context, gpointer user_data)
    {
        return static_cast<DisplayState *>(user_data)->draw(widget, context);
    }

    gboolean draw(GtkWidget *widget, cairo_t *context)
    {
        if (!overlay_enabled_ || display_timing_.pts == GST_CLOCK_TIME_NONE)
            return FALSE;

        GtkAllocation allocation;
        gtk_widget_get_allocation(widget, &allocation);
        const auto content = ssv_display_content_rect(
            {allocation.width, allocation.height,
             gtk_widget_get_scale_factor(widget)},
            video_size_);
        if (!content)
            return FALSE;

        const auto frame = presentation_.present(display_timing_);
        cairo_save(context);
        cairo_rectangle(
            context, content->x, content->y, content->width, content->height);
        cairo_clip(context);
        cairo_set_line_width(context, 2.0);
        cairo_select_font_face(
            context,
            "Sans",
            CAIRO_FONT_SLANT_NORMAL,
            font_face_ == "bold"
                ? CAIRO_FONT_WEIGHT_BOLD
                : CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(context, font_size_);
        for (const auto &box : frame.boxes)
            draw_box(context, *content, box);
        cairo_restore(context);
        return FALSE;
    }

    void draw_box(
        cairo_t *context,
        const SsvDisplayRect &content,
        const SsvOverlayBox &box) const
    {
        const auto mapped = ssv_display_map_bbox(content, box.detection);
        if (!mapped)
            return;
        if (box.predicted)
            cairo_set_source_rgba(context, 0.15, 0.72, 1.0, 0.95);
        else
            cairo_set_source_rgba(context, 0.15, 0.95, 0.38, 0.95);
        cairo_rectangle(
            context, mapped->x, mapped->y, mapped->width, mapped->height);
        cairo_stroke(context);

        const auto label = ssv_display_overlay_label(box);
        cairo_text_extents_t extents;
        cairo_text_extents(context, label.c_str(), &extents);
        const double text_x = mapped->x;
        const double baseline = std::max(
            content.y + extents.height,
            mapped->y - 4.0);
        cairo_set_source_rgba(context, 0.0, 0.0, 0.0, 0.72);
        cairo_rectangle(
            context,
            text_x - 2.0,
            baseline - extents.height - 2.0,
            extents.width + 4.0,
            extents.height + 4.0);
        cairo_fill(context);
        cairo_set_source_rgba(context, 1.0, 1.0, 1.0, 1.0);
        cairo_move_to(context, text_x, baseline);
        cairo_show_text(context, label.c_str());
    }

    void destroy_widgets() noexcept
    {
        if (window_ == nullptr)
            return;
        if (std::this_thread::get_id() != main_thread_) {
            g_warning(
                "SsvDisplayWindow was destroyed outside its owning GTK "
                "thread; widget references cannot be released safely");
            return;
        }
        closing_ = true;
        gtk_widget_destroy(window_);
        g_object_unref(window_);
        window_ = nullptr;
        overlay_ = nullptr;
        drawing_area_ = nullptr;
        on_close_ = {};
    }

    std::shared_ptr<SsvSourceContext> source_context_;
    std::string source_id_;
    bool overlay_enabled_;
    std::string font_face_;
    int font_size_;
    OverlayPresentationModel presentation_;
    std::thread::id main_thread_;
    GtkWidget *window_ = nullptr;
    GtkWidget *overlay_ = nullptr;
    GtkWidget *drawing_area_ = nullptr;
    std::function<void()> on_close_;
    SsvFrameTiming display_timing_;
    SsvVideoSize video_size_;
    std::mutex mailbox_mutex_;
    DisplayUpdate pending_;
    bool dispatch_pending_ = false;
    bool closed_ = false;
    bool closing_ = false;
};

struct DisplayProbeContext {
    DisplayProbeContext(
        std::shared_ptr<DisplayState> display_state,
        std::shared_ptr<SsvSourceContext> source_context)
        : state(std::move(display_state))
        , source_context(std::move(source_context))
        , timeline(this->source_context->meta())
    {
    }

    ~DisplayProbeContext()
    {
        try {
            timeline.on_lifecycle_reset();
        } catch (...) {
        }
    }

    std::shared_ptr<DisplayState> state;
    std::shared_ptr<SsvSourceContext> source_context;
    SsvTimelineCursor timeline;
};

GstPadProbeReturn display_timing_probe(
    GstPad *, GstPadProbeInfo *info, gpointer user_data)
{
    auto &probe = *static_cast<DisplayProbeContext *>(user_data);
    if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM)
        != 0) {
        auto *event = gst_pad_probe_info_get_event(info);
        if (event != nullptr && GST_EVENT_TYPE(event) == GST_EVENT_CAPS) {
            GstCaps *caps = nullptr;
            gst_event_parse_caps(event, &caps);
            GstVideoInfo video_info;
            gst_video_info_init(&video_info);
            if (caps != nullptr
                && gst_video_info_from_caps(&video_info, caps)) {
                probe.state->post({
                    .timing = std::nullopt,
                    .video_size = SsvVideoSize {
                        GST_VIDEO_INFO_WIDTH(&video_info),
                        GST_VIDEO_INFO_HEIGHT(&video_info),
                        GST_VIDEO_INFO_PAR_N(&video_info),
                        GST_VIDEO_INFO_PAR_D(&video_info),
                    },
                });
            }
        } else if (event != nullptr
            && GST_EVENT_TYPE(event) == GST_EVENT_SEGMENT) {
            const GstSegment *segment = nullptr;
            gst_event_parse_segment(event, &segment);
            if (segment != nullptr && segment->format == GST_FORMAT_TIME) {
                probe.timeline.on_segment({
                    segment->start,
                    segment->time,
                    segment->base,
                    segment->rate,
                });
            }
        } else if (event != nullptr
            && GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_STOP) {
            gboolean reset_time = FALSE;
            gst_event_parse_flush_stop(event, &reset_time);
            probe.timeline.on_flush_stop(reset_time);
        }
    }
    if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) != 0) {
        auto *buffer = gst_pad_probe_info_get_buffer(info);
        if (buffer != nullptr) {
            const auto update = probe.timeline.on_buffer(
                GST_BUFFER_PTS(buffer),
                GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DISCONT));
            probe.state->post({
                .timing = SsvFrameTiming {
                    GST_BUFFER_PTS(buffer),
                    GST_BUFFER_DURATION(buffer),
                    update.generation,
                },
                .video_size = std::nullopt,
            });
        }
    }
    return GST_PAD_PROBE_OK;
}

void destroy_display_probe(gpointer data)
{
    delete static_cast<DisplayProbeContext *>(data);
}

std::string_view expected_sink_factory(SsvResolvedDisplayBackend backend)
{
    return backend == SsvResolvedDisplayBackend::GtkGlSink
        ? "gtkglsink"
        : "gtksink";
}

} // namespace

SsvDisplayWindowError::SsvDisplayWindowError(
    std::string stage,
    std::string message)
    : std::runtime_error(std::move(message))
    , stage_(std::move(stage))
{
}

const std::string &SsvDisplayWindowError::stage() const noexcept
{
    return stage_;
}

class SsvDisplayWindow::Impl {
public:
    Impl(
        const SsvDisplayWindowSpec &spec,
        const SsvDisplayAttachment &attachment)
        : state_(std::make_shared<DisplayState>(spec))
    {
        try {
            build(spec, attachment);
        } catch (...) {
            close();
            throw;
        }
    }

    ~Impl() { close(); }

    void show(std::function<void()> on_close)
    {
        state_->show(std::move(on_close));
    }

    void close() noexcept
    {
        if (timing_pad_ != nullptr && timing_probe_id_ != 0) {
            gst_pad_remove_probe(timing_pad_, timing_probe_id_);
            timing_probe_id_ = 0;
        }
        timing_pad_ = nullptr;
        if (state_ != nullptr)
            state_->close();
    }

private:
    void build(
        const SsvDisplayWindowSpec &spec,
        const SsvDisplayAttachment &attachment)
    {
        GstElement *sink = attachment.sink();
        timing_pad_ = attachment.timing_pad();
        if (sink == nullptr || timing_pad_ == nullptr) {
            throw SsvDisplayWindowError(
                "display.attachment",
                "display attachment has no sink or timing pad");
        }

        const auto *factory = gst_element_get_factory(sink);
        const auto *factory_name = factory != nullptr
            ? gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory))
            : nullptr;
        if (factory_name == nullptr
            || expected_sink_factory(spec.backend) != factory_name) {
            throw SsvDisplayWindowError(
                "display.gtk.widget",
                "resolved display backend does not match attachment sink");
        }
        const auto *widget_property = g_object_class_find_property(
            G_OBJECT_GET_CLASS(sink), "widget");
        if (widget_property == nullptr
            || (widget_property->flags & G_PARAM_READABLE) == 0) {
            throw SsvDisplayWindowError(
                "display.gtk.widget",
                "display sink does not expose a readable widget");
        }

        GtkWidget *video_widget = nullptr;
        g_object_get(sink, "widget", &video_widget, nullptr);
        std::unique_ptr<GtkWidget, decltype(&g_object_unref)>
            video_widget_lease(video_widget, g_object_unref);
        if (!GTK_IS_WIDGET(video_widget)) {
            throw SsvDisplayWindowError(
                "display.gtk.widget",
                "display sink returned an invalid widget");
        }
        state_->build_widgets(video_widget);

        auto context = std::make_unique<DisplayProbeContext>(
            state_, state_->source_context());
        timing_probe_id_ = gst_pad_add_probe(
            timing_pad_,
            static_cast<GstPadProbeType>(
                GST_PAD_PROBE_TYPE_BUFFER
                | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM),
            display_timing_probe,
            context.get(),
            destroy_display_probe);
        if (timing_probe_id_ == 0) {
            throw SsvDisplayWindowError(
                "display.timing", "failed to install display timing probe");
        }
        static_cast<void>(context.release());
    }

    std::shared_ptr<DisplayState> state_;
    GstPad *timing_pad_ = nullptr;
    gulong timing_probe_id_ = 0;
};

void SsvDisplayWindow::initialize(SsvGlBackend gl_backend)
{
    switch (gl_backend) {
    case SsvGlBackend::Auto:
        break;
    case SsvGlBackend::X11:
        gdk_set_allowed_backends("x11");
        break;
    case SsvGlBackend::Wayland:
        gdk_set_allowed_backends("wayland");
        break;
    }
    if (!gtk_init_check(nullptr, nullptr)) {
        throw SsvDisplayWindowError(
            "display.gtk.init", "GTK could not open a display");
    }
}

std::unique_ptr<SsvDisplayWindow> SsvDisplayWindow::create(
    SsvDisplayWindowSpec spec,
    const SsvDisplayAttachment *attachment)
{
    if (spec.source_context != nullptr
        && spec.source_context->source_id() != spec.source_id) {
        throw SsvDisplayWindowError(
            "display.source-context",
            "display source-context does not match source-id");
    }
    if (attachment == nullptr) {
        throw SsvDisplayWindowError(
            "display.attachment",
            "display window requires a display attachment");
    }
    if (spec.source_id.empty()) {
        throw SsvDisplayWindowError(
            "display.gtk.window", "display source ID must not be empty");
    }
    return std::unique_ptr<SsvDisplayWindow>(
        new SsvDisplayWindow(
            std::make_unique<Impl>(spec, *attachment)));
}

SsvDisplayWindow::SsvDisplayWindow(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl))
{
}

SsvDisplayWindow::~SsvDisplayWindow() = default;

void SsvDisplayWindow::show(std::function<void()> on_close)
{
    impl_->show(std::move(on_close));
}

void SsvDisplayWindow::close() noexcept
{
    impl_->close();
}

} // namespace ssv
