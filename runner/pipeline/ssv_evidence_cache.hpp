#pragma once

#include "ssv_config.hpp"
#include "ssv_meta.hpp"

#include <gst/gst.h>

#include <memory>
#include <string>

namespace ssv {

/// 持有单个 pipeline source 的可选编码视频证据缓存。
///
/// 缓存观察 parser 输出，并根据 splitmuxsink bus 消息完成 sidecar。
/// 它不持有 pipeline 本身。
class SsvEvidenceCache final {
public:
    SsvEvidenceCache(
        SsvEvidenceCacheConfig config,
        std::string source_id,
        std::shared_ptr<SsvSourceContext> source_context);
    ~SsvEvidenceCache();

    SsvEvidenceCache(const SsvEvidenceCache &) = delete;
    SsvEvidenceCache &operator=(const SsvEvidenceCache &) = delete;

    /// 配置 splitmuxsink 并安装 parser 观察 probe。
    /// 必须在 pipeline 处于 NULL 或 READY 状态时调用。
    void configure(GstElement *parser, GstElement *splitmux_sink);

    /// 消费 splitmuxsink element 消息。失败与 pipeline bus callback 隔离，
    /// 只会使该可选缓存不可用。
    void handle_message(GstMessage *message) noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ssv
