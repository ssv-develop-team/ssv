#pragma once

#include <gst/base/gstbasetransform.h>

G_BEGIN_DECLS

#define SSV_TYPE_INFER (ssv_infer_get_type())
G_DECLARE_FINAL_TYPE(SsvInfer, ssv_infer, SSV, INFER, GstBaseTransform)

G_END_DECLS
