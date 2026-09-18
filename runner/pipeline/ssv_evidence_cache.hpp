#pragma once

#include "ssv_config.hpp"
#include "ssv_meta.hpp"

#include <gst/gst.h>

#include <memory>
#include <string>

namespace ssv {

/// Owns the optional encoded-video evidence cache for one pipeline source.
///
/// The cache observes the parser output and finalizes sidecars from
/// splitmuxsink bus messages. It never owns the pipeline itself.
class SsvEvidenceCache final {
public:
    SsvEvidenceCache(
        SsvEvidenceCacheConfig config,
        std::string source_id,
        std::shared_ptr<SsvSourceContext> source_context);
    ~SsvEvidenceCache();

    SsvEvidenceCache(const SsvEvidenceCache &) = delete;
    SsvEvidenceCache &operator=(const SsvEvidenceCache &) = delete;

    /// Configures splitmuxsink and installs the parser observation probe.
    /// Must be called while the pipeline is in NULL or READY state.
    void configure(GstElement *parser, GstElement *splitmux_sink);

    /// Consumes splitmuxsink element messages. Failures are isolated from the
    /// pipeline bus callback and only make this optional cache unavailable.
    void handle_message(GstMessage *message) noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ssv
