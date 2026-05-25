#pragma once

#include <gst/gst.h>

/// Initialize a GST debug category with consistent SSV naming convention.
/// Usage in plugin .cpp:
///   GST_DEBUG_CATEGORY_STATIC(ssv_template_debug);
///   // in plugin_init:
///   SSV_GST_DEBUG_INIT(ssv_template_debug, "ssv-template");
#define SSV_GST_DEBUG_INIT(category, name) \
    GST_DEBUG_CATEGORY_INIT(category, name, 0, "SSV " name " plugin")
