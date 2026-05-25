#include "gstssvtemplate.hpp"
#include "ssv_config.hpp"
#include "ssv_logging.hpp"

#include <gst/video/video.h>

#include <new>

GST_DEBUG_CATEGORY_STATIC(ssv_template_debug);

struct _SsvTemplate {
    GstBaseTransform parent;
    ssv::SsvConfig *config;
};

G_DEFINE_TYPE(SsvTemplate, ssv_template, GST_TYPE_BASE_TRANSFORM)

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE(
    "sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS(
        "video/x-raw, format=(string){ I420, BGR, RGB }, "
        "width=(int)[1, MAX], height=(int)[1, MAX], "
        "framerate=(fraction)[0, MAX]"
    )
);

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
    "src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS(
        "video/x-raw, format=(string){ I420, BGR, RGB }, "
        "width=(int)[1, MAX], height=(int)[1, MAX], "
        "framerate=(fraction)[0, MAX]"
    )
);

static gboolean
ssv_template_start(GstBaseTransform *trans) {
    SsvTemplate *self = SSV_TEMPLATE(trans);

    self->config = new (std::nothrow) ssv::SsvConfig();
    if (!self->config)
        return FALSE;

    try {
        *self->config = ssv::ssv_config_load();
        GST_INFO_OBJECT(self, "loaded config: analysis_fps=%d, frame=%dx%d",
            self->config->analysis_fps,
            self->config->frame_width,
            self->config->frame_height);
    } catch (const std::exception &e) {
        GST_WARNING_OBJECT(self, "config load failed, using defaults: %s", e.what());
    }

    return TRUE;
}

static gboolean
ssv_template_stop(GstBaseTransform *trans) {
    SsvTemplate *self = SSV_TEMPLATE(trans);
    delete self->config;
    self->config = nullptr;
    return TRUE;
}

static GstFlowReturn
ssv_template_transform_ip(GstBaseTransform *trans, GstBuffer *buf) {
    SsvTemplate *self = SSV_TEMPLATE(trans);

    GST_DEBUG_OBJECT(self, "passing buffer timestamp %" GST_TIME_FORMAT,
        GST_TIME_ARGS(GST_BUFFER_TIMESTAMP(buf)));

    return GST_FLOW_OK;
}

static void
ssv_template_class_init(SsvTemplateClass *klass) {
    GstBaseTransformClass *base_class = GST_BASE_TRANSFORM_CLASS(klass);
    GstElementClass *element_class = GST_ELEMENT_CLASS(klass);

    gst_element_class_set_static_metadata(element_class,
        "SSV Template",
        "Generic/Video",
        "SSV pass-through template element for build verification",
        "site-safety-vision");

    gst_element_class_add_static_pad_template(element_class, &sink_template);
    gst_element_class_add_static_pad_template(element_class, &src_template);

    base_class->start = ssv_template_start;
    base_class->stop = ssv_template_stop;
    base_class->transform_ip = ssv_template_transform_ip;
}

static void
ssv_template_init(SsvTemplate *self) {
    self->config = nullptr;
}

GST_ELEMENT_REGISTER_DEFINE(ssv_template, "ssvtemplate",
    GST_RANK_NONE, SSV_TYPE_TEMPLATE)

static gboolean
plugin_init(GstPlugin *plugin) {
    SSV_GST_DEBUG_INIT(ssv_template_debug, "ssv-template");
    return GST_ELEMENT_REGISTER(ssv_template, plugin);
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    ssvtemplate,
    "SSV Template Plugin",
    plugin_init,
    "0.1.0",
    "LGPL",
    "site-safety-vision",
    "https://github.com/site-safety-vision/site-safety-vision"
)
