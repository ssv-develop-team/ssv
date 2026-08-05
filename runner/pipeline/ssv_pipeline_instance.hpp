#pragma once

#include "ssv_meta.hpp"

#include <gst/gst.h>

#include <functional>
#include <memory>
#include <optional>

namespace ssv {

class SsvPipelineBuilder;

enum class SsvPipelineMessageOrigin {
    Other,
    DisplayBranch,
    DisplaySink,
};

struct SsvGstElementDeleter {
    SsvGstElementDeleter() = default;
    void operator()(GstElement *element) const noexcept;

private:
    using BeforeUnref = std::function<void()>;

    friend class SsvPipelineBuilder;
    explicit SsvGstElementDeleter(BeforeUnref before_unref);

    BeforeUnref before_unref_;
};

using SsvPipelinePtr =
    std::unique_ptr<GstElement, SsvGstElementDeleter>;

class SsvDisplayAttachment final {
public:
    /// Retains references to both borrowed objects.
    SsvDisplayAttachment(GstElement *sink, GstPad *timing_pad);
    ~SsvDisplayAttachment();

    SsvDisplayAttachment(const SsvDisplayAttachment &) = delete;
    SsvDisplayAttachment &operator=(const SsvDisplayAttachment &) = delete;
    /// Transfers both retained references. Handles borrowed from `other`
    /// remain valid while this object retains them.
    SsvDisplayAttachment(SsvDisplayAttachment &&other) noexcept;
    SsvDisplayAttachment &operator=(SsvDisplayAttachment &&) = delete;

    /// Returns a borrowed handle. A move transfers its lifetime guarantee to
    /// the destination.
    [[nodiscard]] GstElement *sink() const noexcept;
    /// Returns a borrowed handle. A move transfers its lifetime guarantee to
    /// the destination.
    [[nodiscard]] GstPad *timing_pad() const noexcept;

private:
    void reset() noexcept;

    GstElement *sink_ = nullptr;
    GstPad *timing_pad_ = nullptr;
};

class SsvPipelineInstance final {
public:
    /// Takes the pipeline and optional attachment. Attachment objects must be
    /// descendants of the pipeline.
    explicit SsvPipelineInstance(
        SsvPipelinePtr pipeline,
        std::optional<SsvDisplayAttachment> display_attachment = std::nullopt,
        std::shared_ptr<SsvSourceContext> source_context = {});
    ~SsvPipelineInstance();

    SsvPipelineInstance(const SsvPipelineInstance &) = delete;
    SsvPipelineInstance &operator=(const SsvPipelineInstance &) = delete;
    SsvPipelineInstance(SsvPipelineInstance &&other) noexcept;
    SsvPipelineInstance &operator=(SsvPipelineInstance &&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept;
    /// Returns a borrowed handle valid until reset, destruction, or a move.
    [[nodiscard]] GstElement *pipeline() const noexcept;
    /// Returns a borrowed view valid until reset, destruction, or a move.
    [[nodiscard]] const SsvDisplayAttachment *display_attachment()
        const noexcept;
    /// Returns an owning copy of the source context used by this pipeline.
    [[nodiscard]] std::shared_ptr<SsvSourceContext> source_context()
        const noexcept;
    /// Classifies a borrowed message without transferring ownership.
    [[nodiscard]] SsvPipelineMessageOrigin message_origin(
        const GstMessage *message) const noexcept;

    /// Releases the display attachment before releasing the pipeline.
    void reset() noexcept;

private:
    // Declared before pipeline_ so destruction releases the pipeline first,
    // then the context that is borrowed by its plugin instances.
    std::shared_ptr<SsvSourceContext> source_context_;
    SsvPipelinePtr pipeline_;
    std::optional<SsvDisplayAttachment> display_attachment_;
};

} // namespace ssv
