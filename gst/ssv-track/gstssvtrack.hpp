#pragma once

#include <gst/base/gstbasetransform.h>

G_BEGIN_DECLS

#define SSV_TYPE_TRACK (ssv_track_get_type())
G_DECLARE_FINAL_TYPE(SsvTrack, ssv_track, SSV, TRACK, GstBaseTransform)

G_END_DECLS
