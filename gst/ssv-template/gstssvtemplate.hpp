#pragma once

#include <gst/base/gstbasetransform.h>

G_BEGIN_DECLS

#define SSV_TYPE_TEMPLATE (ssv_template_get_type())
G_DECLARE_FINAL_TYPE(SsvTemplate, ssv_template, SSV, TEMPLATE, GstBaseTransform)

GST_ELEMENT_REGISTER_DECLARE(ssv_template)

G_END_DECLS
