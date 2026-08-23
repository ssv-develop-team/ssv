#include "ssv_event_log.hpp"

#include <algorithm>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <unistd.h>

namespace ssv {
namespace {

std::string encode_value(std::string_view value)
{
    const bool safe = !value.empty() && std::ranges::all_of(
        value, [](unsigned char character) {
            return (character >= 'a' && character <= 'z')
                || (character >= 'A' && character <= 'Z')
                || (character >= '0' && character <= '9')
                || character == '_'
                || character == '-'
                || character == '.'
                || character == '/'
                || character == ':';
        });
    if (safe)
        return std::string(value);

    std::string encoded = "\"";
    constexpr char hex_digits[] = "0123456789abcdef";
    for (const unsigned char character : value) {
        switch (character) {
        case '\\': encoded += "\\\\"; break;
        case '"': encoded += "\\\""; break;
        case '\n': encoded += "\\n"; break;
        case '\r': encoded += "\\r"; break;
        case '\t': encoded += "\\t"; break;
        default:
            if (character < 0x20 || character == 0x7f) {
                encoded += "\\u00";
                encoded += hex_digits[(character >> 4) & 0x0f];
                encoded += hex_digits[character & 0x0f];
            } else {
                encoded += static_cast<char>(character);
            }
            break;
        }
    }
    encoded += '"';
    return encoded;
}

struct SsvEventNameVisitor {
    std::string_view operator()(const SsvRuntimeResolvedEvent &) const noexcept
    {
        return "runtime_resolved";
    }

    std::string_view operator()(
        const SsvAccelerationFallbackEvent &) const noexcept
    {
        return "acceleration_fallback";
    }

    std::string_view operator()(
        const SsvBufferContractFailedEvent &) const noexcept
    {
        return "buffer_contract_failed";
    }

    std::string_view operator()(const SsvInferenceStatsEvent &) const noexcept
    {
        return "inference_stats";
    }

    std::string_view operator()(const SsvFatalErrorEvent &) const noexcept
    {
        return "fatal_error";
    }

    std::string_view operator()(const SsvNativeDiagnosticEvent &) const noexcept
    {
        return "native_diagnostic";
    }
};

std::string_view event_name(const SsvEventPayload &payload) noexcept
{
    return std::visit(SsvEventNameVisitor {}, payload);
}

std::string encode_event(const SsvEvent &event)
{
    const auto *runtime = std::get_if<SsvRuntimeResolvedEvent>(
        &event.payload);
    const auto *contract = std::get_if<SsvBufferContractFailedEvent>(
        &event.payload);
    const auto *inference = std::get_if<SsvInferenceStatsEvent>(
        &event.payload);
    const auto *fatal = std::get_if<SsvFatalErrorEvent>(&event.payload);
    const auto *native = std::get_if<SsvNativeDiagnosticEvent>(
        &event.payload);
    std::string record = "event=";
    record += event_name(event.payload);
    record += " source_id=" + encode_value(event.context.source_id);
    if (event.context.run_attempt_id) {
        record += " run_attempt_id="
            + std::to_string(*event.context.run_attempt_id);
    }
    if (runtime != nullptr) {
        record += " decoder=" + encode_value(runtime->decoder)
            + " va_device=" + encode_value(runtime->va_device)
            + " va_driver=" + encode_value(runtime->va_driver)
            + " decode_memory=" + encode_value(runtime->decode_memory)
            + " vpp=" + encode_value(runtime->vpp)
            + " display_backend=" + encode_value(runtime->display_backend)
            + " egl_renderer=" + encode_value(runtime->egl_renderer)
            + " provider_chain=" + encode_value(runtime->provider_chain)
            + " provider_device=" + encode_value(runtime->provider_device)
            + " precision=" + encode_value(runtime->precision)
            + " model_hash=" + encode_value(runtime->model_hash)
            + " input_contract=" + encode_value(runtime->input_contract)
            + " cache_status=" + encode_value(runtime->cache_status);
    } else if (contract != nullptr) {
        record += " stage=" + encode_value(contract->stage)
            + " expected_caps=" + encode_value(contract->expected_caps)
            + " expected_allocator="
                + encode_value(contract->expected_allocator)
            + " expected_memory=" + encode_value(contract->expected_memory)
            + " actual_caps=" + encode_value(contract->actual_caps)
            + " actual_allocator="
                + encode_value(contract->actual_allocator)
            + " actual_memory=" + encode_value(contract->actual_memory)
            + " reason=" + encode_value(contract->reason);
    } else if (inference != nullptr) {
        const auto duration_milliseconds = [](std::chrono::microseconds value) {
            return static_cast<double>(value.count()) / 1'000.0;
        };
        const auto latency_milliseconds = [](const SsvLatencyPercentiles &value) {
            if (!value.p50 || !value.p95)
                return std::string("unmeasured");
            std::ostringstream formatted;
            formatted.imbue(std::locale::classic());
            formatted << std::fixed << std::setprecision(3)
                      << static_cast<double>(value.p50->count()) / 1'000.0
                      << '/' << static_cast<double>(value.p95->count()) / 1'000.0;
            return formatted.str();
        };
        std::ostringstream latency;
        latency.imbue(std::locale::classic());
        latency << "p50/p95"
                << " queue=" << latency_milliseconds(inference->queue)
                << " decode=" << latency_milliseconds(inference->decode)
                << " color_resize=" << latency_milliseconds(inference->color_resize)
                << " normalize_layout="
                << latency_milliseconds(inference->normalize_layout)
                << " backend_h2d=" << latency_milliseconds(inference->backend_h2d)
                << " backend_execution="
                << latency_milliseconds(inference->backend_execution)
                << " backend_d2h=" << latency_milliseconds(inference->backend_d2h)
                << " backend_unattributed="
                << latency_milliseconds(inference->backend_unattributed)
                << " postprocess=" << latency_milliseconds(inference->postprocess)
                << " total=" << latency_milliseconds(inference->total);
        std::ostringstream fields;
        fields.imbue(std::locale::classic());
        fields << " frames=" << inference->completed
               << '/' << inference->received
               << " dropped=" << inference->dropped
               << " fps=" << std::fixed << std::setprecision(3)
               << inference->completed_fps
               << " max_gap_ms="
               << std::fixed << std::setprecision(3)
               << duration_milliseconds(inference->longest_result_gap)
               << " latency_ms=" << encode_value(latency.str());
        record += fields.str();
    } else if (fatal != nullptr) {
        record += " exit_code="
            + std::to_string(static_cast<int>(fatal->exit_code))
            + " stage=" + encode_value(fatal->stage)
            + " error=" + encode_value(fatal->error);
    } else if (native != nullptr) {
        const auto source_severity = native->source_severity
                == SsvNativeDiagnosticSeverity::Error
            ? "error"
            : "warning";
        record += " origin=" + encode_value(native->origin)
            + " source_severity=" + source_severity
            + " domain=" + encode_value(native->domain)
            + " code=" + std::to_string(native->code)
            + " message=" + encode_value(native->message);
        if (native->debug)
            record += " debug=" + encode_value(*native->debug);
    } else {
        const auto &fallback = std::get<SsvAccelerationFallbackEvent>(
            event.payload);
        record += " from=" + encode_value(fallback.from)
            + " to=" + encode_value(fallback.to)
            + " stage=" + encode_value(fallback.stage)
            + " reason=" + encode_value(fallback.reason);
    }
    record += '\n';
    return record;
}

struct EncodedField {
    std::string name;
    std::string value;
};

std::vector<EncodedField> parse_encoded_fields(std::string_view record)
{
    std::vector<EncodedField> fields;
    const auto content_end = record.ends_with('\n')
        ? record.size() - 1
        : record.size();
    std::size_t position = 0;
    while (position < content_end) {
        const auto equals = record.find('=', position);
        if (equals == std::string_view::npos || equals >= content_end)
            throw std::runtime_error("invalid encoded log field");

        std::size_t value_end = equals + 1;
        if (value_end < content_end && record[value_end] == '"') {
            ++value_end;
            bool escaped = false;
            while (value_end < content_end) {
                const char character = record[value_end++];
                if (escaped) {
                    escaped = false;
                } else if (character == '\\') {
                    escaped = true;
                } else if (character == '"') {
                    break;
                }
            }
        } else {
            const auto space = record.find(' ', value_end);
            value_end = space == std::string_view::npos
                ? content_end
                : std::min(space, content_end);
        }

        fields.push_back({
            .name = std::string(record.substr(position, equals - position)),
            .value = std::string(record.substr(
                equals + 1, value_end - equals - 1)),
        });
        position = value_end;
        if (position < content_end) {
            if (record[position] != ' ')
                throw std::runtime_error("invalid encoded log separator");
            ++position;
        }
    }
    return fields;
}

constexpr std::size_t pretty_field_width = 15;

void append_padded(
    std::string &record,
    std::string_view value,
    std::size_t width)
{
    record.append(value);
    if (value.size() < width)
        record.append(width - value.size(), ' ');
}

void append_pretty_group(std::string &record, std::string_view name)
{
    record += "  ";
    record.append(name);
    record += ":\n";
}

void append_pretty_field(
    std::string &record,
    std::string_view name,
    std::string_view value)
{
    record += "    ";
    append_padded(record, name, pretty_field_width);
    record += " = ";
    record.append(value);
    record += '\n';
}

std::string format_pretty_milliseconds(std::chrono::microseconds value)
{
    std::ostringstream formatted;
    formatted.imbue(std::locale::classic());
    formatted << std::fixed << std::setprecision(3)
              << static_cast<double>(value.count()) / 1'000.0;
    return formatted.str();
}

std::string format_pretty_fps(double value)
{
    std::ostringstream formatted;
    formatted.imbue(std::locale::classic());
    formatted << std::fixed << std::setprecision(3) << value;
    return formatted.str();
}

void append_pretty_latency_header(std::string &record)
{
    record += "    ";
    append_padded(record, "metric", pretty_field_width);
    record += "  p50       p95\n";
}

void append_pretty_latency_row(
    std::string &record,
    std::string_view name,
    const SsvLatencyPercentiles &percentiles)
{
    record += "    ";
    append_padded(record, name, pretty_field_width);
    record += "  ";
    if (!percentiles.p50 || !percentiles.p95) {
        record += "unmeasured\n";
        return;
    }
    const auto p50 = format_pretty_milliseconds(*percentiles.p50);
    const auto p95 = format_pretty_milliseconds(*percentiles.p95);
    append_padded(record, p50, 9);
    record.append(p95);
    record += '\n';
}

std::string encode_pretty_event(const SsvEvent &event)
{
    const auto *runtime = std::get_if<SsvRuntimeResolvedEvent>(
        &event.payload);
    const auto *inference = std::get_if<SsvInferenceStatsEvent>(
        &event.payload);

    std::string record = "event=";
    record.append(event_name(event.payload));
    record += " source_id=" + encode_value(event.context.source_id);
    if (event.context.run_attempt_id) {
        record += " run_attempt_id="
            + std::to_string(*event.context.run_attempt_id);
    }
    record += '\n';

    if (runtime != nullptr) {
        append_pretty_group(record, "decode");
        append_pretty_field(record, "decoder", encode_value(runtime->decoder));
        append_pretty_field(
            record, "va_device", encode_value(runtime->va_device));
        append_pretty_field(
            record, "va_driver", encode_value(runtime->va_driver));
        append_pretty_field(
            record, "decode_memory", encode_value(runtime->decode_memory));
        append_pretty_field(record, "vpp", encode_value(runtime->vpp));

        append_pretty_group(record, "display");
        append_pretty_field(
            record, "display_backend", encode_value(runtime->display_backend));
        append_pretty_field(
            record, "egl_renderer", encode_value(runtime->egl_renderer));

        append_pretty_group(record, "inference");
        append_pretty_field(
            record, "provider_chain", encode_value(runtime->provider_chain));
        append_pretty_field(
            record, "provider_device", encode_value(runtime->provider_device));
        append_pretty_field(record, "precision", encode_value(runtime->precision));

        append_pretty_group(record, "model");
        append_pretty_field(record, "model_hash", encode_value(runtime->model_hash));
        append_pretty_field(
            record, "input_contract", encode_value(runtime->input_contract));
        append_pretty_field(
            record, "cache_status", encode_value(runtime->cache_status));
        return record;
    }

    if (inference != nullptr) {
        append_pretty_group(record, "throughput");
        append_pretty_field(
            record,
            "frames",
            std::to_string(inference->completed) + '/'
                + std::to_string(inference->received));
        append_pretty_field(
            record, "dropped", std::to_string(inference->dropped));
        append_pretty_field(
            record, "fps", format_pretty_fps(inference->completed_fps));
        append_pretty_field(
            record,
            "max_gap_ms",
            format_pretty_milliseconds(inference->longest_result_gap));

        append_pretty_group(record, "latency_ms");
        append_pretty_latency_header(record);
        append_pretty_latency_row(record, "queue", inference->queue);
        append_pretty_latency_row(record, "decode", inference->decode);
        append_pretty_latency_row(
            record, "color_resize", inference->color_resize);
        append_pretty_latency_row(
            record, "normalize_layout", inference->normalize_layout);
        append_pretty_latency_row(
            record, "backend_h2d", inference->backend_h2d);
        append_pretty_latency_row(
            record, "backend_execution", inference->backend_execution);
        append_pretty_latency_row(
            record, "backend_d2h", inference->backend_d2h);
        append_pretty_latency_row(
            record, "backend_unattributed", inference->backend_unattributed);
        append_pretty_latency_row(record, "postprocess", inference->postprocess);
        append_pretty_latency_row(record, "total", inference->total);
        return record;
    }

    append_pretty_group(record, "details");
    for (const auto &field : parse_encoded_fields(encode_event(event))) {
        if (field.name == "event" || field.name == "source_id"
            || field.name == "run_attempt_id") {
            continue;
        }
        append_pretty_field(record, field.name, field.value);
    }
    return record;
}

std::string render_fields(const std::vector<EncodedField> &fields)
{
    std::string record;
    for (const auto &field : fields) {
        if (!record.empty())
            record += ' ';
        record += field.name + '=' + field.value;
    }
    record += '\n';
    return record;
}

std::size_t encoded_unit_end(
    std::string_view value,
    std::size_t position,
    std::size_t interior_end) noexcept
{
    const auto byte = static_cast<unsigned char>(value[position]);
    if (byte == '\\') {
        if (position + 1 < interior_end && value[position + 1] == 'u'
            && position + 6 <= interior_end) {
            return position + 6;
        }
        return std::min(position + 2, interior_end);
    }
    if (byte < 0x80)
        return position + 1;

    std::size_t width = 1;
    if ((byte & 0xe0) == 0xc0)
        width = 2;
    else if ((byte & 0xf0) == 0xe0)
        width = 3;
    else if ((byte & 0xf8) == 0xf0)
        width = 4;
    return std::min(position + width, interior_end);
}

std::string truncate_encoded_value(
    std::string_view value,
    std::size_t target_bytes)
{
    if (value.size() <= target_bytes)
        return std::string(value);
    if (!value.starts_with('"') || !value.ends_with('"')) {
        const auto kept = std::max<std::size_t>(1, target_bytes);
        return std::string(value.substr(0, std::min(kept, value.size())));
    }
    if (target_bytes <= 2)
        return "\"\"";

    const auto interior_end = value.size() - 1;
    const auto interior_budget = target_bytes - 2;
    std::size_t position = 1;
    std::size_t kept_end = 1;
    while (position < interior_end) {
        const auto unit_end = encoded_unit_end(value, position, interior_end);
        if (unit_end - 1 > interior_budget)
            break;
        kept_end = unit_end;
        position = unit_end;
    }
    return '"' + std::string(value.substr(1, kept_end - 1)) + '"';
}

bool is_classification_field(std::string_view name) noexcept
{
    return name == "event"
        || name == "run_attempt_id"
        || name == "exit_code"
        || name == "code"
        || name == "source_severity";
}

std::string render_truncated_fields(
    const std::vector<EncodedField> &fields,
    std::size_t original_bytes)
{
    auto record = render_fields(fields);
    const auto omitted = original_bytes - record.size();
    record.pop_back();
    record += " record_truncated=true omitted_bytes="
        + std::to_string(omitted) + '\n';
    return record;
}

std::string truncate_record(
    std::string_view record,
    std::size_t max_record_bytes)
{
    auto fields = parse_encoded_fields(record);
    std::vector<std::size_t> candidates;
    const auto add_named_candidate = [&](std::string_view name) {
        for (std::size_t index = fields.size(); index > 0; --index) {
            if (fields[index - 1].name == name) {
                candidates.push_back(index - 1);
                return;
            }
        }
    };
    add_named_candidate("debug");
    add_named_candidate("reason");
    add_named_candidate("error");
    add_named_candidate("message");
    for (std::size_t index = fields.size(); index > 0; --index) {
        const auto candidate = index - 1;
        if (is_classification_field(fields[candidate].name)
            || std::ranges::find(candidates, candidate) != candidates.end()) {
            continue;
        }
        candidates.push_back(candidate);
    }

    auto truncated = render_truncated_fields(fields, record.size());
    while (truncated.size() > max_record_bytes) {
        const auto excess = truncated.size() - max_record_bytes;
        bool shortened = false;
        for (const auto candidate : candidates) {
            auto &value = fields[candidate].value;
            const auto target = value.size() > excess
                ? value.size() - excess
                : 0;
            auto replacement = truncate_encoded_value(value, target);
            if (replacement.size() < value.size()) {
                value = std::move(replacement);
                shortened = true;
                break;
            }
        }
        if (!shortened)
            throw std::runtime_error("log field names exceed record limit");
        truncated = render_truncated_fields(fields, record.size());
    }
    return truncated;
}

SsvEventSeverity event_severity(const SsvEvent &event) noexcept
{
    if (std::holds_alternative<SsvFatalErrorEvent>(event.payload))
        return SsvEventSeverity::Fatal;
    if (const auto *native = std::get_if<SsvNativeDiagnosticEvent>(
            &event.payload)) {
        return native->source_severity == SsvNativeDiagnosticSeverity::Error
            ? SsvEventSeverity::Error
            : SsvEventSeverity::Warning;
    }
    return std::holds_alternative<SsvRuntimeResolvedEvent>(event.payload)
            || std::holds_alternative<SsvInferenceStatsEvent>(event.payload)
        ? SsvEventSeverity::Info
        : SsvEventSeverity::Warning;
}

void emergency_write_fatal_failure(SsvEventLogStats &stats) noexcept
{
    constexpr char message[] =
        "event=fatal_error log_delivery_failed=true\n";
    ++stats.emergency_write_attempts;
    const auto written = ::write(
        STDERR_FILENO, message, sizeof(message) - 1);
    if (written != static_cast<ssize_t>(sizeof(message) - 1))
        ++stats.emergency_write_failures;
}

} // namespace

std::unique_ptr<SsvEventLog> SsvEventLog::create(
    SsvEventLogOptions options,
    std::unique_ptr<SsvLogSink> sink)
{
    if (!sink)
        throw std::invalid_argument("SsvEventLog requires a sink");
    if (options.max_record_bytes < 1024) {
        throw std::invalid_argument(
            "SsvEventLog max_record_bytes must be at least 1024");
    }
    if (options.fatal_flush_timeout <= std::chrono::milliseconds::zero()
        || options.shutdown_timeout <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument(
            "SsvEventLog timeouts must be positive");
    }
    if (options.fatal_flush_timeout > options.shutdown_timeout) {
        throw std::invalid_argument(
            "SsvEventLog fatal flush timeout must not exceed shutdown timeout");
    }
    return std::unique_ptr<SsvEventLog>(
        new SsvEventLog(options, std::move(sink)));
}

SsvEventLog::SsvEventLog(
    SsvEventLogOptions options,
    std::unique_ptr<SsvLogSink> sink) noexcept
    : options_(options), sink_(std::move(sink))
{
}

SsvEventLog::~SsvEventLog() noexcept
{
    static_cast<void>(close());
}

void SsvEventLog::emit(SsvEvent event) noexcept
{
    try {
        std::lock_guard lock(mutex_);
        ++stats_.emit_calls;
        if (state_ != State::Open) {
            ++stats_.rejected_after_close;
            return;
        }

        const auto severity = event_severity(event);
        if (severity != SsvEventSeverity::Fatal
            && severity < options_.minimum_severity) {
            ++stats_.filtered;
            return;
        }

        std::string bytes;
        try {
            bytes = encode_event(event);
            if (bytes.size() > options_.max_record_bytes) {
                bytes = truncate_record(bytes, options_.max_record_bytes);
                ++stats_.truncated;
            } else if (options_.format == SsvEventLogFormat::Pretty) {
                try {
                    auto pretty = encode_pretty_event(event);
                    if (pretty.size() <= options_.max_record_bytes)
                        bytes = std::move(pretty);
                } catch (...) {
                    // Keep the valid structured record if presentation fails.
                }
            }
        } catch (...) {
            ++stats_.encode_failed;
            if (severity == SsvEventSeverity::Fatal)
                emergency_write_fatal_failure(stats_);
            return;
        }

        SsvEncodedLogRecord record {
            .bytes = std::move(bytes),
            .sequence = next_sequence_++,
            .emitted_at = std::chrono::system_clock::now(),
            .severity = severity,
            .force_flush = severity == SsvEventSeverity::Fatal,
        };
        bool fatal_delivery_failed = false;
        switch (sink_->submit(std::move(record))) {
        case SsvLogSubmitResult::Accepted:
            ++stats_.accepted_by_sink;
            break;
        case SsvLogSubmitResult::DroppedBackpressure:
            ++stats_.submit_dropped;
            fatal_delivery_failed = severity == SsvEventSeverity::Fatal;
            break;
        case SsvLogSubmitResult::Failed:
            ++stats_.submit_failed;
            fatal_delivery_failed = severity == SsvEventSeverity::Fatal;
            break;
        }
        if (severity == SsvEventSeverity::Fatal) {
            const auto result = sink_->flush(
                std::chrono::steady_clock::now()
                + options_.fatal_flush_timeout);
            stats_.written_records = result.stats.written_records;
            stats_.async_dropped_records =
                result.stats.async_dropped_records;
            stats_.write_failed_records = result.stats.write_failed_records;
            stats_.final_flush_status = result.status;
            fatal_delivery_failed = fatal_delivery_failed
                || result.status != SsvLogIoStatus::Completed;
            if (fatal_delivery_failed)
                emergency_write_fatal_failure(stats_);
        }
    } catch (...) {
        // Logging failures must not escape into the runtime path.
    }
}

SsvEventLogStats SsvEventLog::close() noexcept
{
    try {
        std::lock_guard lock(mutex_);
        if (state_ == State::Closed)
            return stats_;

        state_ = State::Closing;
        const auto result = sink_->close(
            std::chrono::steady_clock::now() + options_.shutdown_timeout);
        stats_.written_records = result.stats.written_records;
        stats_.async_dropped_records = result.stats.async_dropped_records;
        stats_.write_failed_records = result.stats.write_failed_records;
        stats_.final_close_status = result.status;
        state_ = State::Closed;
        return stats_;
    } catch (...) {
        SsvEventLogStats failed;
        failed.final_close_status = SsvLogIoStatus::Failed;
        return failed;
    }
}

} // namespace ssv
