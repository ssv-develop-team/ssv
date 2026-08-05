#include "ssv_pipeline_contract.hpp"

#include <gst/video/video.h>

#include <algorithm>
#include <cctype>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace ssv {
namespace {

constexpr auto kContractFailureDetails = "ssv-buffer-contract-failed";
constexpr auto kContractReadyDetails = "ssv-buffer-contract-ready";

std::string_view format_name(SsvPixelFormat format)
{
    switch (format) {
    case SsvPixelFormat::Nv12: return "NV12";
    case SsvPixelFormat::Rgba: return "RGBA";
    case SsvPixelFormat::Bgrx: return "BGRx";
    }
    return "unknown";
}

std::string_view memory_name(SsvMemoryKind memory)
{
    switch (memory) {
    case SsvMemoryKind::SystemMemory: return "SystemMemory";
    case SsvMemoryKind::VaMemory: return "VAMemory";
    case SsvMemoryKind::DmaBuf: return "DMABuf";
    case SsvMemoryKind::GlMemory: return "GLMemory";
    case SsvMemoryKind::Unknown: return "unknown";
    }
    return "unknown";
}

bool allows(
    const SsvPipelineContractExpectation &expectation,
    SsvMemoryKind memory)
{
    return std::ranges::find(expectation.allowed_memories, memory)
        != expectation.allowed_memories.end();
}

bool requires_allocator_observation(
    const SsvPipelineContractExpectation &expectation)
{
    return std::ranges::any_of(
        expectation.allowed_memories,
        [](SsvMemoryKind memory) {
            return memory != SsvMemoryKind::SystemMemory;
        });
}

std::string memories_text(const std::vector<SsvMemoryKind> &memories)
{
    std::ostringstream output;
    for (std::size_t index = 0; index < memories.size(); ++index) {
        if (index != 0)
            output << '/';
        output << memory_name(memories[index]);
    }
    return output.str();
}

std::string caps_text(
    std::optional<SsvPixelFormat> format,
    const std::vector<SsvMemoryKind> &memories,
    int width,
    int height)
{
    std::ostringstream output;
    output << "video/x-raw";
    if (!memories.empty())
        output << "(memory:" << memories_text(memories) << ')';
    output << ",format="
           << (format ? format_name(*format) : std::string_view("unknown"));
    if (width > 0 && height > 0)
        output << ",width=" << width << ",height=" << height;
    return output.str();
}

std::string expectation_caps_text(
    const SsvPipelineContractExpectation &expectation)
{
    return caps_text(
        expectation.format,
        expectation.allowed_memories,
        expectation.width,
        expectation.height);
}

std::string observation_caps_text(
    const SsvPipelineContractObservation &observation)
{
    return caps_text(
        observation.format,
        observation.caps_memories,
        observation.width,
        observation.height);
}

std::optional<SsvPipelineContractViolation> violation(
    const SsvPipelineContractExpectation &expectation,
    const SsvPipelineContractObservation &observation,
    std::string message)
{
    return SsvPipelineContractViolation {
        expectation.boundary,
        expectation_caps_text(expectation),
        requires_allocator_observation(expectation)
            ? memories_text(expectation.allowed_memories)
            : "not-required",
        memories_text(expectation.allowed_memories),
        observation_caps_text(observation),
        observation.allocator_memory
            ? std::string(memory_name(*observation.allocator_memory))
            : "unobserved",
        observation.buffer_memories.empty()
            ? "unobserved"
            : memories_text(observation.buffer_memories),
        std::move(message),
    };
}

std::optional<SsvPixelFormat> pixel_format(GstVideoFormat format)
{
    switch (format) {
    case GST_VIDEO_FORMAT_NV12: return SsvPixelFormat::Nv12;
    case GST_VIDEO_FORMAT_RGBA: return SsvPixelFormat::Rgba;
    case GST_VIDEO_FORMAT_BGRx: return SsvPixelFormat::Bgrx;
    default: return std::nullopt;
    }
}

SsvMemoryKind memory_from_text(std::string_view value)
{
    std::string lower(value);
    std::ranges::transform(lower, lower.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    if (lower.find("dmabuf") != std::string::npos)
        return SsvMemoryKind::DmaBuf;
    if (lower.find("vamemory") != std::string::npos
        || lower.find("vasurface") != std::string::npos) {
        return SsvMemoryKind::VaMemory;
    }
    if (lower.find("glmemory") != std::string::npos)
        return SsvMemoryKind::GlMemory;
    if (lower.find("systemmemory") != std::string::npos
        || lower == "system") {
        return SsvMemoryKind::SystemMemory;
    }
    return SsvMemoryKind::Unknown;
}

std::vector<SsvMemoryKind> caps_memories(const GstCaps *caps)
{
    std::vector<SsvMemoryKind> memories;
    if (caps == nullptr || gst_caps_is_empty(caps))
        return memories;
    const auto *features = gst_caps_get_features(caps, 0);
    if (features == nullptr || gst_caps_features_get_size(features) == 0) {
        memories.push_back(SsvMemoryKind::SystemMemory);
        return memories;
    }
    if (gst_caps_features_is_any(features)) {
        memories.push_back(SsvMemoryKind::Unknown);
        return memories;
    }
    const auto count = gst_caps_features_get_size(features);
    bool has_memory_feature = false;
    for (guint index = 0; index < count; ++index) {
        const auto *feature = gst_caps_features_get_nth(features, index);
        if (feature == nullptr || !g_str_has_prefix(feature, "memory:"))
            continue;
        has_memory_feature = true;
        const auto memory = memory_from_text(feature);
        if (std::ranges::find(memories, memory) == memories.end())
            memories.push_back(memory);
    }
    if (!has_memory_feature)
        memories.push_back(SsvMemoryKind::SystemMemory);
    return memories;
}

SsvMemoryKind memory_kind(GstMemory *memory)
{
    if (memory == nullptr)
        return SsvMemoryKind::Unknown;
    constexpr std::string_view known_types[] {
        "DMABuf", "dmabuf", "VAMemory", "VASurface",
        "GLMemory", "SystemMemory",
    };
    for (const auto type : known_types) {
        if (gst_memory_is_type(memory, type.data()))
            return memory_from_text(type);
    }
    if (memory->allocator != nullptr
        && memory->allocator->mem_type != nullptr) {
        return memory_from_text(memory->allocator->mem_type);
    }
    return SsvMemoryKind::Unknown;
}

struct ContractProbeContext {
    ContractProbeContext()
    {
        g_weak_ref_init(&error_source, nullptr);
    }

    SsvPipelineContractExpectation expectation;
    SsvPipelineContractObservation observation;
    std::function<void(int, int)> on_geometry;
    GWeakRef error_source;
    std::mutex mutex;
    guint buffers_observed = 0;
    bool failed = false;
    bool ready = false;

    ~ContractProbeContext()
    {
        g_weak_ref_clear(&error_source);
    }
};

void post_violation(
    ContractProbeContext &context,
    const SsvPipelineContractViolation &violation)
{
    if (context.failed)
        return;
    auto *error_source = GST_ELEMENT(g_weak_ref_get(&context.error_source));
    if (error_source == nullptr)
        return;
    context.failed = true;
    const std::string detail = violation.message
        + "; expected_caps=" + violation.expected_caps
        + "; expected_allocator=" + violation.expected_allocator
        + "; expected_memory=" + violation.expected_memory
        + "; actual_caps=" + violation.actual_caps
        + "; actual_allocator=" + violation.actual_allocator
        + "; actual_memory=" + violation.actual_memory;
    GError *error = g_error_new_literal(
        ssv_pipeline_contract_error_quark(), 1, detail.c_str());
    GstStructure *details = gst_structure_new(
        kContractFailureDetails,
        "boundary", G_TYPE_INT, static_cast<int>(violation.boundary),
        "expected-caps", G_TYPE_STRING, violation.expected_caps.c_str(),
        "expected-allocator", G_TYPE_STRING,
        violation.expected_allocator.c_str(),
        "expected-memory", G_TYPE_STRING, violation.expected_memory.c_str(),
        "actual-caps", G_TYPE_STRING, violation.actual_caps.c_str(),
        "actual-allocator", G_TYPE_STRING,
        violation.actual_allocator.c_str(),
        "actual-memory", G_TYPE_STRING, violation.actual_memory.c_str(),
        "message", G_TYPE_STRING, violation.message.c_str(),
        nullptr);
    GstMessage *message = gst_message_new_error_with_details(
        GST_OBJECT(error_source), error, detail.c_str(), details);
    g_error_free(error);
    gst_element_post_message(error_source, message);
    gst_object_unref(error_source);
}

bool validate_observation(ContractProbeContext &context)
{
    const auto violation = ssv_pipeline_contract_validate(
        context.expectation, context.observation);
    if (violation) {
        post_violation(context, *violation);
        return false;
    }
    return true;
}

void post_ready(ContractProbeContext &context)
{
    if (context.ready)
        return;
    auto *error_source = GST_ELEMENT(g_weak_ref_get(&context.error_source));
    if (error_source == nullptr)
        return;
    context.ready = true;
    GstStructure *details = gst_structure_new(
        kContractReadyDetails,
        "boundary", G_TYPE_INT,
        static_cast<int>(context.expectation.boundary),
        nullptr);
    gst_element_post_message(
        error_source,
        gst_message_new_element(GST_OBJECT(error_source), details));
    gst_object_unref(error_source);
}

void observe_allocator(
    ContractProbeContext &context,
    GstPad *pad,
    GstCaps *caps)
{
    std::unique_ptr<GstQuery, decltype(&gst_query_unref)> query(
        gst_query_new_allocation(caps, FALSE), gst_query_unref);
    if (!gst_pad_peer_query(pad, query.get()))
        return;
    const auto count = gst_query_get_n_allocation_params(query.get());
    if (count == 0)
        return;
    GstAllocator *allocator = nullptr;
    GstAllocationParams params;
    gst_query_parse_nth_allocation_param(
        query.get(), 0, &allocator, &params);
    if (allocator != nullptr && allocator->mem_type != nullptr) {
        context.observation.allocator_memory =
            memory_from_text(allocator->mem_type);
    }
}

GstPadProbeReturn contract_probe(
    GstPad *pad,
    GstPadProbeInfo *info,
    gpointer user_data)
{
    auto &context = *static_cast<ContractProbeContext *>(user_data);
    std::lock_guard<std::mutex> lock(context.mutex);
    if (context.failed)
        return GST_PAD_PROBE_OK;

    if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM)
        != 0) {
        auto *event = gst_pad_probe_info_get_event(info);
        if (event != nullptr && GST_EVENT_TYPE(event) == GST_EVENT_CAPS) {
            GstCaps *caps = nullptr;
            gst_event_parse_caps(event, &caps);
            GstVideoInfo video_info;
            gst_video_info_init(&video_info);
            if (caps != nullptr
                && gst_video_info_from_caps(&video_info, caps)) {
                context.buffers_observed = 0;
                context.observation.allocator_memory.reset();
                context.observation.buffer_memories.clear();
                context.observation.format = pixel_format(
                    GST_VIDEO_INFO_FORMAT(&video_info));
                context.observation.width =
                    GST_VIDEO_INFO_WIDTH(&video_info);
                context.observation.height =
                    GST_VIDEO_INFO_HEIGHT(&video_info);
                context.observation.caps_memories = caps_memories(caps);
                if (context.on_geometry) {
                    try {
                        context.on_geometry(
                            context.observation.width,
                            context.observation.height);
                    } catch (const std::exception &error) {
                        post_violation(context, {
                            context.expectation.boundary,
                            expectation_caps_text(context.expectation),
                            requires_allocator_observation(context.expectation)
                                ? memories_text(
                                      context.expectation.allowed_memories)
                                : "not-required",
                            memories_text(
                                context.expectation.allowed_memories),
                            observation_caps_text(context.observation),
                            context.observation.allocator_memory
                                ? std::string(memory_name(
                                      *context.observation.allocator_memory))
                                : "unobserved",
                            context.observation.buffer_memories.empty()
                                ? "unobserved"
                                : memories_text(
                                      context.observation.buffer_memories),
                            std::string("source geometry update failed: ")
                                + error.what(),
                        });
                        return GST_PAD_PROBE_OK;
                    }
                }
                observe_allocator(context, pad, caps);
            }
        }
    }
    if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) != 0
        && context.buffers_observed < 3) {
        ++context.buffers_observed;
        context.observation.buffer_memories.clear();
        auto *buffer = gst_pad_probe_info_get_buffer(info);
        const auto count = buffer != nullptr
            ? gst_buffer_n_memory(buffer)
            : 0;
        for (guint index = 0; index < count; ++index) {
            const auto kind = memory_kind(
                gst_buffer_peek_memory(buffer, index));
            if (std::ranges::find(
                    context.observation.buffer_memories, kind)
                == context.observation.buffer_memories.end()) {
                context.observation.buffer_memories.push_back(kind);
            }
        }
        if (context.observation.buffer_memories.empty())
            context.observation.buffer_memories.push_back(
                SsvMemoryKind::Unknown);
        if (validate_observation(context))
            post_ready(context);
    }
    return GST_PAD_PROBE_OK;
}

void destroy_contract_probe(gpointer data)
{
    delete static_cast<ContractProbeContext *>(data);
}

} // namespace

std::optional<SsvPipelineContractViolation>
ssv_pipeline_contract_validate(
    const SsvPipelineContractExpectation &expectation,
    const SsvPipelineContractObservation &observation)
{
    if (expectation.allowed_memories.empty()) {
        return violation(
            expectation, observation, "expected memory set is empty");
    }
    if (!observation.format || *observation.format != expectation.format) {
        return violation(
            expectation, observation, "pixel format does not match");
    }
    if (observation.caps_memories.empty()) {
        return violation(
            expectation, observation, "caps memory was not observed");
    }
    if (expectation.width > 0 && expectation.height > 0
        && (observation.width != expectation.width
            || observation.height != expectation.height)) {
        return violation(
            expectation, observation, "frame dimensions do not match");
    }
    if (std::ranges::any_of(
            observation.caps_memories,
            [&](SsvMemoryKind memory) { return !allows(expectation, memory); })) {
        return violation(
            expectation, observation, "caps memory does not match");
    }
    if (requires_allocator_observation(expectation)
        && !observation.allocator_memory) {
        return violation(
            expectation, observation, "allocator memory was not observed");
    }
    if (observation.allocator_memory
        && !allows(expectation, *observation.allocator_memory)) {
        return violation(
            expectation, observation, "allocator memory does not match");
    }
    if (std::ranges::any_of(
            observation.buffer_memories,
            [&](SsvMemoryKind memory) { return !allows(expectation, memory); })) {
        return violation(
            expectation, observation, "buffer memory does not match");
    }
    return std::nullopt;
}

SsvPipelineContractRecovery ssv_pipeline_contract_recovery(
    const SsvPipelinePlan &plan) noexcept
{
    if (plan.decode.software_fallback_allowed
        && plan.decode.backend != SsvDecodeBackend::Software) {
        return SsvPipelineContractRecovery::FallbackSoftware;
    }
    return SsvPipelineContractRecovery::Fatal;
}

GQuark ssv_pipeline_contract_error_quark() noexcept
{
    return g_quark_from_static_string("ssv-pipeline-contract-error");
}

std::optional<SsvPipelineContractViolation>
ssv_pipeline_contract_violation_from_message(const GstMessage *message)
{
    if (message == nullptr || GST_MESSAGE_TYPE(message) != GST_MESSAGE_ERROR)
        return std::nullopt;
    const GstStructure *details = nullptr;
    gst_message_parse_error_details(
        const_cast<GstMessage *>(message), &details);
    if (details == nullptr
        || !gst_structure_has_name(details, kContractFailureDetails)) {
        return std::nullopt;
    }

    gint boundary = 0;
    if (!gst_structure_get_int(details, "boundary", &boundary)
        || boundary < static_cast<int>(SsvPipelineBoundary::DecodeTee)
        || boundary > static_cast<int>(SsvPipelineBoundary::AnalysisHost)) {
        return std::nullopt;
    }
    const auto required = [&](const char *field) -> const char * {
        return gst_structure_get_string(details, field);
    };
    const char *expected_caps = required("expected-caps");
    const char *expected_allocator = required("expected-allocator");
    const char *expected_memory = required("expected-memory");
    const char *actual_caps = required("actual-caps");
    const char *actual_allocator = required("actual-allocator");
    const char *actual_memory = required("actual-memory");
    const char *failure_message = required("message");
    if (expected_caps == nullptr || expected_allocator == nullptr
        || expected_memory == nullptr || actual_caps == nullptr
        || actual_allocator == nullptr || actual_memory == nullptr
        || failure_message == nullptr) {
        return std::nullopt;
    }
    return SsvPipelineContractViolation {
        static_cast<SsvPipelineBoundary>(boundary),
        expected_caps,
        expected_allocator,
        expected_memory,
        actual_caps,
        actual_allocator,
        actual_memory,
        failure_message,
    };
}

std::optional<SsvPipelineBoundary>
ssv_pipeline_contract_ready_from_message(const GstMessage *message) noexcept
{
    if (message == nullptr || GST_MESSAGE_TYPE(message) != GST_MESSAGE_ELEMENT)
        return std::nullopt;
    const GstStructure *details = gst_message_get_structure(
        const_cast<GstMessage *>(message));
    if (details == nullptr
        || !gst_structure_has_name(details, kContractReadyDetails)) {
        return std::nullopt;
    }
    gint boundary = 0;
    if (!gst_structure_get_int(details, "boundary", &boundary)
        || boundary < static_cast<int>(SsvPipelineBoundary::DecodeTee)
        || boundary > static_cast<int>(SsvPipelineBoundary::AnalysisHost)) {
        return std::nullopt;
    }
    return static_cast<SsvPipelineBoundary>(boundary);
}

namespace pipeline_internal {

void watch_contract(
    GstElement *error_source,
    GstPad *pad,
    SsvPipelineContractExpectation expectation,
    std::function<void(int width, int height)> on_geometry)
{
    if (error_source == nullptr || pad == nullptr)
        throw std::invalid_argument("contract probe requires element and pad");
    auto context = std::make_unique<ContractProbeContext>();
    context->expectation = std::move(expectation);
    context->on_geometry = std::move(on_geometry);
    g_weak_ref_set(&context->error_source, G_OBJECT(error_source));
    const auto probe_id = gst_pad_add_probe(
        pad,
        static_cast<GstPadProbeType>(
            GST_PAD_PROBE_TYPE_BUFFER
            | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM),
        contract_probe,
        context.get(),
        destroy_contract_probe);
    if (probe_id == 0)
        throw std::runtime_error("failed to install pipeline contract probe");
    static_cast<void>(context.release());
}

} // namespace pipeline_internal

} // namespace ssv
