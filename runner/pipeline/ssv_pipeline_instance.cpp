#include "ssv_pipeline_instance.hpp"

#include <stdexcept>
#include <string_view>
#include <utility>

namespace ssv {

SsvGstElementDeleter::SsvGstElementDeleter(BeforeUnref before_unref)
    : before_unref_(std::move(before_unref))
{
}

void SsvGstElementDeleter::operator()(GstElement *element) const noexcept
{
    if (element == nullptr)
        return;
    if (before_unref_)
        before_unref_();
    gst_object_unref(element);
}

SsvDisplayAttachment::SsvDisplayAttachment(
    GstElement *sink,
    GstPad *timing_pad)
{
    if (sink == nullptr || !GST_IS_ELEMENT(sink)
        || timing_pad == nullptr || !GST_IS_PAD(timing_pad)) {
        throw std::invalid_argument(
            "display attachment requires a sink and timing pad");
    }
    sink_ = GST_ELEMENT(gst_object_ref(sink));
    timing_pad_ = GST_PAD(gst_object_ref(timing_pad));
}

SsvDisplayAttachment::~SsvDisplayAttachment()
{
    reset();
}

SsvDisplayAttachment::SsvDisplayAttachment(
    SsvDisplayAttachment &&other) noexcept
    : sink_(std::exchange(other.sink_, nullptr))
    , timing_pad_(std::exchange(other.timing_pad_, nullptr))
{
}

GstElement *SsvDisplayAttachment::sink() const noexcept
{
    return sink_;
}

GstPad *SsvDisplayAttachment::timing_pad() const noexcept
{
    return timing_pad_;
}

void SsvDisplayAttachment::reset() noexcept
{
    if (timing_pad_ != nullptr) {
        gst_object_unref(timing_pad_);
        timing_pad_ = nullptr;
    }
    if (sink_ != nullptr) {
        gst_object_unref(sink_);
        sink_ = nullptr;
    }
}

SsvPipelineInstance::SsvPipelineInstance(
    SsvPipelinePtr pipeline,
    std::optional<SsvDisplayAttachment> display_attachment,
    std::shared_ptr<SsvSourceContext> source_context)
    : source_context_(std::move(source_context))
    , pipeline_(std::move(pipeline))
    , display_attachment_(std::move(display_attachment))
{
    if (pipeline_ == nullptr || !GST_IS_PIPELINE(pipeline_.get())) {
        throw std::invalid_argument(
            "pipeline instance requires a GStreamer pipeline");
    }
    if (display_attachment_
        && (!gst_object_has_as_ancestor(
                GST_OBJECT(display_attachment_->sink()),
                GST_OBJECT(pipeline_.get()))
            || !gst_object_has_as_ancestor(
                GST_OBJECT(display_attachment_->timing_pad()),
                GST_OBJECT(pipeline_.get())))) {
        throw std::invalid_argument(
            "display attachment objects must belong to its pipeline");
    }
}

SsvPipelineInstance::~SsvPipelineInstance()
{
    reset();
}

SsvPipelineInstance::SsvPipelineInstance(
    SsvPipelineInstance &&other) noexcept
    : source_context_(std::move(other.source_context_))
    , pipeline_(std::move(other.pipeline_))
    , display_attachment_(std::move(other.display_attachment_))
{
    other.display_attachment_.reset();
}

SsvPipelineInstance::operator bool() const noexcept
{
    return pipeline_ != nullptr;
}

GstElement *SsvPipelineInstance::pipeline() const noexcept
{
    return pipeline_.get();
}

const SsvDisplayAttachment *SsvPipelineInstance::display_attachment()
    const noexcept
{
    return display_attachment_ ? &*display_attachment_ : nullptr;
}

std::shared_ptr<SsvSourceContext> SsvPipelineInstance::source_context()
    const noexcept
{
    return source_context_;
}

SsvPipelineMessageOrigin SsvPipelineInstance::message_origin(
    const GstMessage *message) const noexcept
{
    if (pipeline_ == nullptr || !display_attachment_ || message == nullptr
        || GST_MESSAGE_SRC(message) == nullptr) {
        return SsvPipelineMessageOrigin::Other;
    }
    GstObject *source = GST_MESSAGE_SRC(message);
    if (!gst_object_has_as_ancestor(
            source, GST_OBJECT(pipeline_.get()))) {
        return SsvPipelineMessageOrigin::Other;
    }
    if (source == GST_OBJECT(display_attachment_->sink()))
        return SsvPipelineMessageOrigin::DisplaySink;

    gchar *path = gst_object_get_path_string(source);
    const bool display_branch = path != nullptr
        && std::string_view(path).find("display-") != std::string_view::npos;
    g_free(path);
    return display_branch
        ? SsvPipelineMessageOrigin::DisplayBranch
        : SsvPipelineMessageOrigin::Other;
}

void SsvPipelineInstance::reset() noexcept
{
    display_attachment_.reset();
    pipeline_.reset();
    source_context_.reset();
}

} // namespace ssv
