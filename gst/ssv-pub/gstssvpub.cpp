#include "gstssvpub.hpp"
#include "ssv_logging.hpp"
#include "ssv_meta.hpp"

#include <gst/video/video.h>
#include <hiredis/hiredis.h>
#include <nlohmann/json.hpp>

#include <cstring>
#include <ctime>

GST_DEBUG_CATEGORY_STATIC(ssv_pub_debug);

struct _SsvPub {
    GstBaseTransform parent;

    gchar *redis_host;
    gint redis_port;
    gchar *stream_key;

    redisContext *redis_ctx;
};

enum {
    PROP_0,
    PROP_REDIS_HOST,
    PROP_REDIS_PORT,
    PROP_STREAM_KEY,
};

G_DEFINE_TYPE(SsvPub, ssv_pub, GST_TYPE_BASE_TRANSFORM)

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE(
    "sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS(
        "video/x-raw, format=(string)BGR, "
        "width=(int)[1, MAX], height=(int)[1, MAX], "
        "framerate=(fraction)[0, MAX]"));

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
    "src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS(
        "video/x-raw, format=(string)BGR, "
        "width=(int)[1, MAX], height=(int)[1, MAX], "
        "framerate=(fraction)[0, MAX]"));

// ── Redis helpers ──────────────────────────────────────────────────────

static gboolean
ssv_pub_redis_connect(SsvPub *self) {
    if (self->redis_ctx) {
        redisFree(self->redis_ctx);
        self->redis_ctx = nullptr;
    }

    struct timeval timeout = { 2, 0 };
    self->redis_ctx = redisConnectWithTimeout(self->redis_host, self->redis_port, timeout);

    if (!self->redis_ctx || self->redis_ctx->err) {
        if (self->redis_ctx) {
            GST_ERROR_OBJECT(self, "Redis connect failed: %s", self->redis_ctx->errstr);
            redisFree(self->redis_ctx);
            self->redis_ctx = nullptr;
        } else {
            GST_ERROR_OBJECT(self, "Redis connect failed: allocation error");
        }
        return FALSE;
    }

    GST_INFO_OBJECT(self, "connected to Redis at %s:%d", self->redis_host, self->redis_port);
    return TRUE;
}

static void
ssv_pub_redis_publish(SsvPub *self, const SsvFrameDetections &det) {
    if (!self->redis_ctx)
        return;

    using json = nlohmann::json;

    json detections_arr = json::array();
    for (const auto &d : det.detections) {
        json det_obj = {
            {"class", d.class_name},
            {"class_id", d.class_id},
            {"confidence", d.confidence},
            {"bbox", {d.x1, d.y1, d.x2, d.y2}},
            {"track_id", d.track_id}
        };
        detections_arr.push_back(det_obj);
    }

    json msg = {
        {"type", "detection"},
        {"source", det.source_id},
        {"timestamp_ms", std::time(nullptr) * 1000LL},
        {"frame_id", det.frame_id},
        {"detections", detections_arr}
    };

    std::string payload = msg.dump();

    auto *reply = (redisReply *)redisCommand(self->redis_ctx,
        "XADD %s * event %s",
        self->stream_key, payload.c_str());

    if (!reply) {
        GST_WARNING_OBJECT(self, "Redis XADD failed, reconnecting...");
        ssv_pub_redis_connect(self);
        return;
    }
    if (reply->type == REDIS_REPLY_ERROR) {
        GST_WARNING_OBJECT(self, "Redis error: %s", reply->str);
    }
    freeReplyObject(reply);

    GST_DEBUG_OBJECT(self, "published frame %" G_GUINT64_FORMAT " with %zu detections",
        det.frame_id, det.detections.size());
}

// ── GstBaseTransform callbacks ────────────────────────────────────────

static gboolean
ssv_pub_start(GstBaseTransform *trans) {
    auto *self = SSV_PUB(trans);
    return ssv_pub_redis_connect(self);
}

static gboolean
ssv_pub_stop(GstBaseTransform *trans) {
    auto *self = SSV_PUB(trans);
    if (self->redis_ctx) {
        redisFree(self->redis_ctx);
        self->redis_ctx = nullptr;
    }
    return TRUE;
}

static GstFlowReturn
ssv_pub_transform_ip(GstBaseTransform *trans, GstBuffer *buf) {
    (void)buf;
    auto *self = SSV_PUB(trans);

    auto det = SsvDetectionStore::instance().take();
    if (!det.detections.empty()) {
        ssv_pub_redis_publish(self, det);
    }

    return GST_FLOW_OK;
}

// ── Properties ─────────────────────────────────────────────────────────

static void
ssv_pub_set_property(GObject *object, guint prop_id,
                      const GValue *value, GParamSpec *pspec) {
    auto *self = SSV_PUB(object);
    switch (prop_id) {
    case PROP_REDIS_HOST:
        g_free(self->redis_host);
        self->redis_host = g_value_dup_string(value);
        break;
    case PROP_REDIS_PORT:
        self->redis_port = g_value_get_int(value);
        break;
    case PROP_STREAM_KEY:
        g_free(self->stream_key);
        self->stream_key = g_value_dup_string(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void
ssv_pub_get_property(GObject *object, guint prop_id,
                      GValue *value, GParamSpec *pspec) {
    auto *self = SSV_PUB(object);
    switch (prop_id) {
    case PROP_REDIS_HOST:
        g_value_set_string(value, self->redis_host);
        break;
    case PROP_REDIS_PORT:
        g_value_set_int(value, self->redis_port);
        break;
    case PROP_STREAM_KEY:
        g_value_set_string(value, self->stream_key);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

// ── Class / instance init ──────────────────────────────────────────────

static void
ssv_pub_finalize(GObject *object) {
    auto *self = SSV_PUB(object);
    g_free(self->redis_host);
    g_free(self->stream_key);
    if (self->redis_ctx)
        redisFree(self->redis_ctx);
    G_OBJECT_CLASS(ssv_pub_parent_class)->finalize(object);
}

static void
ssv_pub_class_init(SsvPubClass *klass) {
    auto *gobject_class = G_OBJECT_CLASS(klass);
    auto *base_class = GST_BASE_TRANSFORM_CLASS(klass);
    auto *element_class = GST_ELEMENT_CLASS(klass);

    gobject_class->set_property = ssv_pub_set_property;
    gobject_class->get_property = ssv_pub_get_property;
    gobject_class->finalize = ssv_pub_finalize;

    g_object_class_install_property(gobject_class, PROP_REDIS_HOST,
        g_param_spec_string("redis-host", "Redis Host",
            "Redis server hostname",
            "localhost", (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_REDIS_PORT,
        g_param_spec_int("redis-port", "Redis Port",
            "Redis server port",
            1, 65535, 6379,
            (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_STREAM_KEY,
        g_param_spec_string("stream-key", "Stream Key",
            "Redis Stream key for detection events",
            "ssv:events", (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    gst_element_class_set_static_metadata(element_class,
        "SSV Redis Publisher",
        "Generic/Video",
        "Publish detection events to Redis Streams",
        "site-safety-vision");

    gst_element_class_add_static_pad_template(element_class, &sink_template);
    gst_element_class_add_static_pad_template(element_class, &src_template);

    base_class->start = ssv_pub_start;
    base_class->stop = ssv_pub_stop;
    base_class->transform_ip = ssv_pub_transform_ip;
    base_class->passthrough_on_same_caps = TRUE;
}

static void
ssv_pub_init(SsvPub *self) {
    self->redis_host = g_strdup("localhost");
    self->redis_port = 6379;
    self->stream_key = g_strdup("ssv:events");
    self->redis_ctx = nullptr;
}

// ── Plugin registration ────────────────────────────────────────────────

GST_ELEMENT_REGISTER_DEFINE(ssv_pub, "ssvpub",
    GST_RANK_NONE, SSV_TYPE_PUB)

static gboolean
plugin_init(GstPlugin *plugin) {
    SSV_GST_DEBUG_INIT(ssv_pub_debug, "ssv-pub");
    return GST_ELEMENT_REGISTER(ssv_pub, plugin);
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR, GST_VERSION_MINOR,
    ssvpub,
    "SSV Redis Publisher Plugin",
    plugin_init,
    "0.1.0", "LGPL",
    "site-safety-vision",
    "https://github.com/site-safety-vision/site-safety-vision")
