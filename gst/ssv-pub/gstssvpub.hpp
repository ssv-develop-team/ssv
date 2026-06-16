#pragma once

#include "ssv_meta.hpp"

#include <gst/base/gstbasetransform.h>

#include <cstdint>
#include <string>

std::string ssv_pub_build_event_payload(const SsvFrameDetections &det, std::int64_t timestamp_ms);

G_BEGIN_DECLS

#define SSV_TYPE_PUB (ssv_pub_get_type())
G_DECLARE_FINAL_TYPE(SsvPub, ssv_pub, SSV, PUB, GstBaseTransform)

G_END_DECLS
