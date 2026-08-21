#include "display/ssv_display_window.hpp"
#include "pipeline/ssv_pipeline_instance.hpp"

#include <gtk/gtk.h>
#include <gst/gst.h>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>

namespace {

bool near(double left, double right)
{
    return std::abs(left - right) < 0.001;
}

void mark_finalized(gpointer data, GObject *)
{
    *static_cast<bool *>(data) = true;
}

ssv::SsvDisplayWindowSpec make_display_spec(std::string source_id)
{
    auto source_context = std::make_shared<SsvSourceContext>(source_id);
    return {
        .source_id = std::move(source_id),
        .gl_backend = ssv::SsvGlBackend::Auto,
        .overlay = {},
        .backend = ssv::SsvResolvedDisplayBackend::GtkSink,
        .source_context = std::move(source_context),
    };
}

void test_display_spec_owns_source_and_overlay_values()
{
    std::string source_id = "owned-source";
    ssv::SsvOverlayConfig overlay;
    overlay.enabled = false;
    auto source_context = std::make_shared<SsvSourceContext>(source_id);
    ssv::SsvDisplayWindowSpec spec {
        .source_id = source_id,
        .gl_backend = ssv::SsvGlBackend::Wayland,
        .overlay = overlay,
        .backend = ssv::SsvResolvedDisplayBackend::GtkGlSink,
        .source_context = source_context,
    };

    source_id = "mutated-source";
    overlay.enabled = true;

    assert(spec.source_id == "owned-source");
    assert(!spec.overlay.enabled);
    assert(spec.source_context != nullptr);
    assert(spec.source_context->source_id() == "owned-source");
    assert(spec.gl_backend == ssv::SsvGlBackend::Wayland);
    assert(spec.backend == ssv::SsvResolvedDisplayBackend::GtkGlSink);
}

void test_auto_backend_selection()
{
    using ssv::SsvGlBackend;
    const auto resolve = ssv::ssv_display_resolve_auto_gl_backend;

    assert(resolve("", true, true) == SsvGlBackend::X11);
    assert(resolve("x11", true, true) == SsvGlBackend::X11);
    assert(resolve("wayland", true, true) == SsvGlBackend::Wayland);
    assert(resolve("wayland", true, false) == SsvGlBackend::X11);
    assert(resolve("", true, false) == SsvGlBackend::X11);
    assert(resolve("", false, true) == SsvGlBackend::Wayland);
    assert(resolve("", false, false) == SsvGlBackend::Auto);
}

void test_window_rejects_missing_attachment()
{
    try {
        static_cast<void>(ssv::SsvDisplayWindow::create(
            make_display_spec("missing-attachment"), nullptr));
        assert(false && "display window accepted no attachment");
    } catch (const ssv::SsvDisplayWindowError &error) {
        assert(error.stage() == "display.attachment");
    }
}

void test_window_rejects_mismatched_source_context()
{
    auto spec = make_display_spec("display-source");
    spec.source_context = std::make_shared<SsvSourceContext>(
        "different-source");
    try {
        static_cast<void>(ssv::SsvDisplayWindow::create(spec, nullptr));
        assert(false && "display accepted a mismatched source context");
    } catch (const ssv::SsvDisplayWindowError &error) {
        assert(error.stage() == "display.source-context");
    }
}

void test_window_falls_back_to_source_id_without_context()
{
    auto spec = make_display_spec("display-fallback-source");
    spec.source_context.reset();
    GstElement *sink = gst_element_factory_make("fakesink", nullptr);
    GstPad *timing_pad = gst_pad_new("fallback-timing", GST_PAD_SRC);
    assert(sink != nullptr && timing_pad != nullptr);
    ssv::SsvDisplayAttachment attachment(sink, timing_pad);
    gst_object_unref(sink);
    gst_object_unref(timing_pad);
    try {
        static_cast<void>(ssv::SsvDisplayWindow::create(spec, &attachment));
        assert(false && "display accepted an incompatible sink");
    } catch (const ssv::SsvDisplayWindowError &error) {
        assert(error.stage() == "display.gtk.widget");
    }
}

bool initialize_display_integration()
{
    try {
        ssv::SsvDisplayWindow::initialize(ssv::SsvGlBackend::Auto);
        return true;
    } catch (const ssv::SsvDisplayWindowError &) {
        std::fprintf(
            stderr,
            "display integration skipped: GTK display unavailable\n");
        return false;
    }
}

void test_window_borrow_survives_attachment_move_and_releases_probe()
{
    GstElement *sink = gst_element_factory_make(
        "gtksink", "opaque-widget-output");
    if (sink == nullptr) {
        std::fprintf(
            stderr,
            "display integration skipped: gtksink unavailable\n");
        return;
    }
    GstPad *timing_pad = gst_pad_new("opaque-timing", GST_PAD_SRC);
    assert(timing_pad != nullptr);
    bool sink_finalized = false;
    bool timing_pad_finalized = false;
    g_object_weak_ref(
        G_OBJECT(sink), mark_finalized, &sink_finalized);
    g_object_weak_ref(
        G_OBJECT(timing_pad), mark_finalized, &timing_pad_finalized);

    const std::string source_id = "display-attachment-window-test";
    auto source_meta = ssv_meta(source_id);
    {
        ssv::SsvDisplayAttachment attachment(sink, timing_pad);
        gst_object_unref(timing_pad);
        gst_object_unref(sink);
        auto window = ssv::SsvDisplayWindow::create(
            make_display_spec(source_id), &attachment);
        assert(window != nullptr);

        ssv::SsvDisplayAttachment retained_attachment(
            std::move(attachment));
        assert(attachment.sink() == nullptr);
        assert(attachment.timing_pad() == nullptr);
        assert(retained_attachment.sink() == sink);
        assert(retained_attachment.timing_pad() == timing_pad);
        assert(!sink_finalized && !timing_pad_finalized);

        const auto resets_before_close =
            source_meta->stats().generation_resets;
        window->close();
        assert(source_meta->stats().generation_resets
            == resets_before_close + 1);
        window->close();
        assert(source_meta->stats().generation_resets
            == resets_before_close + 1);
        window.reset();
        assert(!sink_finalized && !timing_pad_finalized);
    }
    assert(sink_finalized);
    assert(timing_pad_finalized);
}

void test_window_show_presents_top_level_window()
{
    GstElement *sink = gst_element_factory_make(
        "gtksink", "show-window-widget-output");
    if (sink == nullptr) {
        std::fprintf(
            stderr,
            "display integration skipped: gtksink unavailable\n");
        return;
    }
    GstPad *timing_pad = gst_pad_new("show-window-timing", GST_PAD_SRC);
    assert(timing_pad != nullptr);
    ssv::SsvDisplayAttachment attachment(sink, timing_pad);
    gst_object_unref(timing_pad);
    gst_object_unref(sink);

    const std::string title = "Site Safety Vision - show-window-test";
    auto spec = make_display_spec("show-window-test");
    auto window = ssv::SsvDisplayWindow::create(spec, &attachment);
    window->show([] {});
    while (g_main_context_iteration(nullptr, FALSE)) {
    }

    bool found_visible_window = false;
    GList *toplevels = gtk_window_list_toplevels();
    for (GList *node = toplevels; node != nullptr; node = node->next) {
        auto *toplevel = GTK_WINDOW(node->data);
        const char *window_title = gtk_window_get_title(toplevel);
        if (window_title != nullptr && title == window_title) {
            found_visible_window = gtk_widget_get_visible(
                GTK_WIDGET(toplevel))
                && gtk_widget_get_mapped(GTK_WIDGET(toplevel));
        }
    }
    g_list_free(toplevels);
    assert(found_visible_window);
    window->close();
}

void test_layout_maps_normalized_boxes_inside_letterboxed_video()
{
    const auto content = ssv::ssv_display_content_rect(
        {.width = 1000, .height = 800, .scale_factor = 2},
        {.width = 1920, .height = 1080});
    assert(content.has_value());
    assert(near(content->x, 0.0));
    assert(near(content->y, 119.0));
    assert(near(content->width, 1000.0));
    assert(near(content->height, 562.5));

    SsvDetection detection;
    detection.x1 = 0.1F;
    detection.y1 = 0.2F;
    detection.x2 = 0.3F;
    detection.y2 = 0.4F;
    const auto mapped = ssv::ssv_display_map_bbox(*content, detection);
    assert(mapped.has_value());
    assert(near(mapped->x, 100.0));
    assert(near(mapped->y, 231.5));
    assert(near(mapped->width, 200.0));
    assert(near(mapped->height, 112.5));
}

void test_layout_keeps_portrait_content_centered_and_device_aligned()
{
    const auto content = ssv::ssv_display_content_rect(
        {.width = 801, .height = 1000, .scale_factor = 2},
        {.width = 1920, .height = 1080});
    assert(content.has_value());
    assert(near(content->x, 0.0));
    assert(near(content->y, 275.0));
    assert(near(content->width, 801.0));
    assert(near(content->height, 450.5));
    assert(near(content->x * 2.0, std::round(content->x * 2.0)));
    assert(near(content->y * 2.0, std::round(content->y * 2.0)));
    assert(near(content->width * 2.0, std::round(content->width * 2.0)));
    assert(near(content->height * 2.0, std::round(content->height * 2.0)));
}

void test_layout_uses_stream_pixel_aspect_ratio()
{
    const auto content = ssv::ssv_display_content_rect(
        {.width = 1000, .height = 1000, .scale_factor = 1},
        {
            .width = 720,
            .height = 576,
            .pixel_aspect_ratio_numerator = 16,
            .pixel_aspect_ratio_denominator = 15,
        });
    assert(content.has_value());
    assert(near(content->x, 0.0));
    assert(near(content->y, 125.0));
    assert(near(content->width, 1000.0));
    assert(near(content->height, 750.0));
}

void test_layout_rejects_non_positive_viewport_or_video_size()
{
    const ssv::SsvDisplayViewport valid_viewport {
        .width = 640,
        .height = 480,
        .scale_factor = 1,
    };
    const ssv::SsvVideoSize valid_video {.width = 1280, .height = 720};
    assert(!ssv::ssv_display_content_rect(
                {.width = 0, .height = 480, .scale_factor = 1},
                valid_video)
                .has_value());
    assert(!ssv::ssv_display_content_rect(
                valid_viewport,
                {.width = 1280, .height = -1})
                .has_value());
    assert(!ssv::ssv_display_content_rect(
                {.width = 640, .height = 480, .scale_factor = 0},
                valid_video)
                .has_value());
    assert(!ssv::ssv_display_content_rect(
                valid_viewport,
                {
                    .width = 1280,
                    .height = 720,
                    .pixel_aspect_ratio_numerator = 0,
                })
                .has_value());
    assert(!ssv::ssv_display_content_rect(
                valid_viewport,
                {
                    .width = 1280,
                    .height = 720,
                    .pixel_aspect_ratio_denominator = 0,
                })
                .has_value());
}

void test_layout_clamps_partially_visible_boxes_and_rejects_invalid_boxes()
{
    const ssv::SsvDisplayRect content {
        .x = 10.0,
        .y = 20.0,
        .width = 100.0,
        .height = 50.0,
    };
    SsvDetection partially_visible;
    partially_visible.x1 = -0.25F;
    partially_visible.y1 = 0.25F;
    partially_visible.x2 = 1.25F;
    partially_visible.y2 = 2.0F;
    const auto mapped =
        ssv::ssv_display_map_bbox(content, partially_visible);
    assert(mapped.has_value());
    assert(near(mapped->x, 10.0));
    assert(near(mapped->y, 32.5));
    assert(near(mapped->width, 100.0));
    assert(near(mapped->height, 37.5));

    SsvDetection reversed;
    reversed.x1 = 0.75F;
    reversed.y1 = 0.25F;
    reversed.x2 = 0.25F;
    reversed.y2 = 0.5F;
    assert(!ssv::ssv_display_map_bbox(content, reversed).has_value());

    SsvDetection non_finite;
    non_finite.x1 = std::numeric_limits<float>::quiet_NaN();
    non_finite.y1 = 0.0F;
    non_finite.x2 = 1.0F;
    non_finite.y2 = 1.0F;
    assert(!ssv::ssv_display_map_bbox(content, non_finite).has_value());
}

void test_overlay_label_always_includes_track_id()
{
    SsvOverlayBox box;
    std::snprintf(
        box.detection.class_name,
        sizeof(box.detection.class_name),
        "person");
    box.detection.confidence = 0.687F;

    box.track_id = 42;
    assert(ssv::ssv_display_overlay_label(box) == "person #42 0.69");

    box.track_id = -1;
    assert(ssv::ssv_display_overlay_label(box) == "person #-1 0.69");
}

} // namespace

int main()
{
    gst_init(nullptr, nullptr);
    test_display_spec_owns_source_and_overlay_values();
    test_auto_backend_selection();
    test_window_rejects_missing_attachment();
    test_window_rejects_mismatched_source_context();
    test_window_falls_back_to_source_id_without_context();
    test_layout_maps_normalized_boxes_inside_letterboxed_video();
    test_layout_keeps_portrait_content_centered_and_device_aligned();
    test_layout_uses_stream_pixel_aspect_ratio();
    test_layout_rejects_non_positive_viewport_or_video_size();
    test_layout_clamps_partially_visible_boxes_and_rejects_invalid_boxes();
    test_overlay_label_always_includes_track_id();
    if (initialize_display_integration()) {
        test_window_borrow_survives_attachment_move_and_releases_probe();
        test_window_show_presents_top_level_window();
    }
    return 0;
}
