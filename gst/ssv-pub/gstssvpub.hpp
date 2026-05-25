#pragma once

#include <gst/base/gstbasetransform.h>

G_BEGIN_DECLS

#define SSV_TYPE_PUB (ssv_pub_get_type())
G_DECLARE_FINAL_TYPE(SsvPub, ssv_pub, SSV, PUB, GstBaseTransform)

G_END_DECLS
