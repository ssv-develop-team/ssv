#include "ssv_evidence_cache.hpp"

#include <fcntl.h>
#include <nlohmann/json.hpp>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ssv {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

constexpr std::string_view kFragmentPrefix = "segment-";
constexpr std::string_view kFragmentSuffix = ".mp4";
constexpr std::string_view kSidecarSchema = "ssv.evidence-segment.v1";
constexpr std::size_t kObservationHistoryCapacity = 16 * 1024;

struct ObservedBuffer {
    GstClockTime pts = GST_CLOCK_TIME_NONE;
    GstClockTime duration = GST_CLOCK_TIME_NONE;
    GstClockTime running_time = GST_CLOCK_TIME_NONE;
    std::uint64_t generation = 0;
    std::int64_t wall_clock_ms = 0;
};

struct FragmentBounds {
    std::optional<GstClockTime> start;
    std::optional<GstClockTime> end;
};

struct SegmentMetadata {
    std::optional<std::uint64_t> generation;
    std::optional<GstClockTime> source_pts_start;
    std::optional<GstClockTime> source_pts_end;
    std::optional<std::int64_t> wall_clock_start_ms;
    std::optional<std::int64_t> wall_clock_end_ms;
};

std::int64_t system_now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string encode_path_component(std::string_view value)
{
    constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size());
    for (const unsigned char character : value) {
        const bool safe = (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9')
            || character == '-' || character == '_' || character == '.';
        if (safe) {
            encoded.push_back(static_cast<char>(character));
            continue;
        }
        encoded.push_back('~');
        encoded.push_back(hex[character >> 4]);
        encoded.push_back(hex[character & 0x0F]);
    }
    return encoded.empty() ? "source" : encoded;
}

bool is_within(const fs::path &path, const fs::path &root)
{
    const auto normalized_path = path.lexically_normal();
    const auto normalized_root = root.lexically_normal();
    auto path_it = normalized_path.begin();
    auto root_it = normalized_root.begin();
    for (; root_it != normalized_root.end(); ++root_it, ++path_it) {
        if (path_it == normalized_path.end() || *path_it != *root_it)
            return false;
    }
    return true;
}

bool is_fragment_name(std::string_view name)
{
    if (!name.starts_with(kFragmentPrefix)
        || !name.ends_with(kFragmentSuffix)) {
        return false;
    }
    const auto digits = name.substr(
        kFragmentPrefix.size(),
        name.size() - kFragmentPrefix.size() - kFragmentSuffix.size());
    if (digits.empty())
        return false;
    return std::all_of(
        digits.begin(), digits.end(),
        [](unsigned char character) {
            return character >= '0' && character <= '9';
        });
}

int next_fragment_index(const fs::path &directory)
{
    int next = 0;
    std::error_code error;
    for (const auto &entry : fs::directory_iterator(directory, error)) {
        if (error)
            break;
        const auto name = entry.path().filename().string();
        if (!is_fragment_name(name))
            continue;
        const auto digits = std::string_view(name).substr(
            kFragmentPrefix.size(),
            name.size() - kFragmentPrefix.size() - kFragmentSuffix.size());
        int index = 0;
        const auto parsed = std::from_chars(
            digits.data(), digits.data() + digits.size(), index);
        if (parsed.ec == std::errc {} && parsed.ptr == digits.data() + digits.size()
            && index < std::numeric_limits<int>::max()) {
            next = std::max(next, index + 1);
        }
    }
    if (error)
        throw std::runtime_error(
            "failed to inspect evidence cache directory: " + error.message());
    return next;
}

std::optional<std::uint64_t> get_uint64(
    const GstStructure *structure,
    const char *field)
{
    guint64 value = 0;
    if (!gst_structure_get_uint64(structure, field, &value)
        || value == GST_CLOCK_TIME_NONE)
        return std::nullopt;
    return value;
}

FragmentBounds fragment_bounds(
    const GstStructure *structure,
    std::optional<GstClockTime> start)
{
    const auto running_time = get_uint64(structure, "running-time");

    FragmentBounds bounds;
    bounds.start = start;
    if (running_time)
        bounds.end = *running_time;
    return bounds;
}

std::optional<std::int64_t> add_duration_ms(
    std::int64_t timestamp_ms,
    GstClockTime duration)
{
    if (duration == GST_CLOCK_TIME_NONE)
        return timestamp_ms;
    const auto duration_ms = static_cast<std::int64_t>(duration / GST_MSECOND);
    if (duration_ms > std::numeric_limits<std::int64_t>::max() - timestamp_ms)
        return std::nullopt;
    return timestamp_ms + duration_ms;
}

bool write_all(int descriptor, std::string_view contents)
{
    const char *cursor = contents.data();
    std::size_t remaining = contents.size();
    while (remaining > 0) {
        const auto written = ::write(descriptor, cursor, remaining);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (written == 0)
            return false;
        cursor += written;
        remaining -= static_cast<std::size_t>(written);
    }
    return true;
}

void fsync_directory(const fs::path &directory)
{
    const int descriptor = ::open(
        directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0)
        throw std::system_error(errno, std::generic_category(),
                                "failed to open evidence cache directory");
    const int result = ::fsync(descriptor);
    const int saved_errno = errno;
    ::close(descriptor);
    if (result != 0)
        throw std::system_error(saved_errno, std::generic_category(),
                                "failed to sync evidence cache directory");
}

} // namespace

class SsvEvidenceCache::Impl final {
public:
    Impl(
        SsvEvidenceCacheConfig config,
        std::string source_id,
        std::shared_ptr<SsvSourceContext> source_context)
        : config_(std::move(config))
        , source_id_(std::move(source_id))
        , source_context_(std::move(source_context))
        , source_directory_(
              fs::path(config_.directory) / encode_path_component(source_id_))
    {
        if (source_id_.empty())
            throw std::invalid_argument("evidence cache source_id must not be empty");
        if (!source_context_)
            throw std::invalid_argument(
                "evidence cache source context must not be null");
        if (!fs::path(config_.directory).is_absolute())
            throw std::invalid_argument(
                "evidence cache directory must be absolute");

        std::error_code error;
        fs::create_directories(source_directory_, error);
        if (error)
            throw std::runtime_error(
                "failed to create evidence cache directory: " + error.message());
        if (!fs::is_directory(source_directory_, error) || error)
            throw std::runtime_error(
                "evidence cache path is not a directory");
        location_pattern_ =
            (source_directory_ / "segment-%08d.mp4").string();
        gst_segment_init(&segment_, GST_FORMAT_TIME);
        timeline_ = std::make_unique<SsvTimelineCursor>(source_context_->meta());
    }

    ~Impl()
    {
        remove_probe();
    }

    void configure(GstElement *parser, GstElement *splitmux_sink)
    {
        if (configured_)
            throw std::logic_error("SsvEvidenceCache may only be configured once");
        if (parser == nullptr || !GST_IS_ELEMENT(parser)
            || splitmux_sink == nullptr || !GST_IS_ELEMENT(splitmux_sink)) {
            throw std::invalid_argument(
                "evidence cache requires parser and splitmuxsink elements");
        }

        auto *parser_src_pad = gst_element_get_static_pad(parser, "src");
        if (parser_src_pad == nullptr) {
            throw std::runtime_error(
                "evidence cache parser has no src pad");
        }

        splitmux_sink_ = splitmux_sink;
        const auto start_index = next_fragment_index(source_directory_);
        g_object_set(
            splitmux_sink_,
            "location", location_pattern_.c_str(),
            "start-index", start_index,
            "max-size-time",
            static_cast<guint64>(config_.segment_duration_ms) * GST_MSECOND,
            "max-size-bytes", static_cast<guint64>(0),
            "max-files", static_cast<guint>(0),
            "async-finalize", FALSE,
            nullptr);

        parser_src_pad_ = parser_src_pad;
        probe_id_ = gst_pad_add_probe(
            parser_src_pad_,
            static_cast<GstPadProbeType>(
                GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM),
            &Impl::on_parser_probe,
            this,
            nullptr);
        if (probe_id_ == 0) {
            gst_object_unref(parser_src_pad_);
            parser_src_pad_ = nullptr;
            throw std::runtime_error(
                "failed to install evidence cache parser probe");
        }
        configured_ = true;
    }

    void handle_message(GstMessage *message) noexcept
    {
        if (!configured_ || unavailable_.load() || message == nullptr
            || GST_MESSAGE_TYPE(message) != GST_MESSAGE_ELEMENT
            || splitmux_sink_ == nullptr
            || GST_MESSAGE_SRC(message) != GST_OBJECT(splitmux_sink_)) {
            return;
        }

        const auto *structure = gst_message_get_structure(message);
        if (structure == nullptr
            || (!gst_structure_has_name(
                    structure, "splitmuxsink-fragment-opened")
                && !gst_structure_has_name(
                    structure, "splitmuxsink-fragment-closed"))) {
            return;
        }

        try {
            if (gst_structure_has_name(
                    structure, "splitmuxsink-fragment-opened")) {
                remember_fragment_start(structure);
            } else {
                finalize_fragment(structure);
            }
        } catch (const std::exception &error) {
            mark_unavailable(error.what());
        } catch (...) {
            mark_unavailable("unknown evidence cache finalization failure");
        }
    }

private:
    static GstPadProbeReturn on_parser_probe(
        GstPad *, GstPadProbeInfo *info, gpointer user_data)
    {
        auto &self = *static_cast<Impl *>(user_data);
        try {
            self.observe_probe(info);
        } catch (const std::exception &error) {
            self.mark_unavailable(error.what());
        } catch (...) {
            self.mark_unavailable("unknown evidence cache probe failure");
        }
        return GST_PAD_PROBE_OK;
    }

    void observe_probe(GstPadProbeInfo *info)
    {
        const auto probe_type = GST_PAD_PROBE_INFO_TYPE(info);
        if ((probe_type & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) != 0) {
            auto *event = gst_pad_probe_info_get_event(info);
            observe_event(event);
        }
        if ((probe_type & GST_PAD_PROBE_TYPE_BUFFER) != 0) {
            auto *buffer = gst_pad_probe_info_get_buffer(info);
            observe_buffer(buffer);
        }
    }

    void observe_event(GstEvent *event)
    {
        if (event == nullptr)
            return;
        switch (GST_EVENT_TYPE(event)) {
        case GST_EVENT_SEGMENT: {
            const GstSegment *segment = nullptr;
            gst_event_parse_segment(event, &segment);
            if (segment == nullptr || segment->format != GST_FORMAT_TIME)
                return;
            timeline_->on_segment({
                segment->start,
                segment->time,
                segment->base,
                segment->rate,
            });
            std::lock_guard<std::mutex> lock(observation_mutex_);
            segment_ = *segment;
            has_segment_ = true;
            return;
        }
        case GST_EVENT_FLUSH_STOP: {
            gboolean reset_time = FALSE;
            gst_event_parse_flush_stop(event, &reset_time);
            timeline_->on_flush_stop(reset_time);
            if (reset_time) {
                std::lock_guard<std::mutex> lock(observation_mutex_);
                has_segment_ = false;
            }
            return;
        }
        default:
            return;
        }
    }

    void observe_buffer(GstBuffer *buffer)
    {
        if (buffer == nullptr)
            return;
        const auto pts = GST_BUFFER_PTS(buffer);
        const auto update = timeline_->on_buffer(
            pts,
            GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DISCONT));
        if (pts == GST_CLOCK_TIME_NONE)
            return;

        GstClockTime running_time = pts;
        {
            std::lock_guard<std::mutex> lock(observation_mutex_);
            if (has_segment_) {
                const auto converted = gst_segment_to_running_time(
                    &segment_, GST_FORMAT_TIME, pts);
                if (converted != GST_CLOCK_TIME_NONE)
                    running_time = converted;
            }
            observations_.push_back({
                pts,
                GST_BUFFER_DURATION(buffer),
                running_time,
                update.generation,
                system_now_ms(),
            });
            while (observations_.size() > kObservationHistoryCapacity)
                observations_.pop_front();
        }
    }

    void remove_probe() noexcept
    {
        if (parser_src_pad_ != nullptr && probe_id_ != 0) {
            gst_pad_remove_probe(parser_src_pad_, probe_id_);
            probe_id_ = 0;
        }
        if (parser_src_pad_ != nullptr) {
            gst_object_unref(parser_src_pad_);
            parser_src_pad_ = nullptr;
        }
    }

    std::vector<ObservedBuffer> observations_for(
        const FragmentBounds &bounds) const
    {
        std::vector<ObservedBuffer> selected;
        std::lock_guard<std::mutex> lock(observation_mutex_);
        for (const auto &observation : observations_) {
            if (bounds.start && observation.running_time < *bounds.start)
                continue;
            // splitmuxsink reports the next fragment boundary on close. Keep
            // the source interval half-open so a buffer at that boundary is
            // owned by the next fragment.
            if (bounds.end && observation.running_time >= *bounds.end)
                continue;
            selected.push_back(observation);
        }
        return selected;
    }

    SegmentMetadata metadata_for(const FragmentBounds &bounds) const
    {
        const auto observations = observations_for(bounds);
        SegmentMetadata metadata;
        if (observations.empty())
            return metadata;

        const auto first = observations.front();
        const auto last = observations.back();
        metadata.generation = first.generation;
        for (const auto &observation : observations) {
            if (observation.generation != *metadata.generation) {
                metadata.generation.reset();
                metadata.source_pts_start.reset();
                metadata.source_pts_end.reset();
                break;
            }
        }

        if (metadata.generation) {
            metadata.source_pts_start = first.pts;
            if (last.duration != GST_CLOCK_TIME_NONE
                && last.pts <= GST_CLOCK_TIME_NONE - last.duration) {
                metadata.source_pts_end = last.pts + last.duration;
            }
        }
        metadata.wall_clock_start_ms = first.wall_clock_ms;
        metadata.wall_clock_end_ms = add_duration_ms(
            last.wall_clock_ms, last.duration);
        return metadata;
    }

    fs::path validated_media_path(const char *location) const
    {
        if (location == nullptr || !is_fragment_name(fs::path(location).filename().string()))
            throw std::runtime_error(
                "splitmuxsink produced an unexpected evidence filename");
        const fs::path media_path(location);
        if (!media_path.is_absolute()
            || media_path.lexically_normal().parent_path()
                != source_directory_.lexically_normal()
            || !is_within(media_path, source_directory_)) {
            throw std::runtime_error(
                "splitmuxsink produced a path outside the evidence cache");
        }
        const auto status = fs::symlink_status(media_path);
        if (status.type() != fs::file_type::regular)
            throw std::runtime_error(
                "finalized evidence media is not a regular file");
        return media_path.lexically_normal();
    }

    void remember_fragment_start(const GstStructure *structure)
    {
        const auto *location = gst_structure_get_string(structure, "location");
        const auto running_time = get_uint64(structure, "running-time");
        if (location == nullptr || !running_time)
            throw std::runtime_error(
                "splitmuxsink opened fragment has no running-time or location");
        std::lock_guard<std::mutex> lock(observation_mutex_);
        fragment_starts_[location] = *running_time;
    }

    void finalize_fragment(const GstStructure *structure)
    {
        const auto *location = gst_structure_get_string(structure, "location");
        auto media_path = validated_media_path(location);
        std::optional<GstClockTime> start;
        {
            std::lock_guard<std::mutex> lock(observation_mutex_);
            if (location != nullptr) {
                const auto found = fragment_starts_.find(location);
                if (found != fragment_starts_.end()) {
                    start = found->second;
                    fragment_starts_.erase(found);
                }
            }
        }
        const auto bounds = fragment_bounds(structure, start);
        if (!bounds.start || !bounds.end || *bounds.end < *bounds.start)
            throw std::runtime_error(
                "splitmuxsink closed fragment has no exact running-time bounds");
        const auto metadata = metadata_for(bounds);

        json sidecar {
            {"schema", kSidecarSchema},
            {"source", source_id_},
            {"stream_generation", metadata.generation
                    ? json(*metadata.generation)
                    : json(nullptr)},
            {"source_pts_start", metadata.source_pts_start
                    ? json(*metadata.source_pts_start)
                    : json(nullptr)},
            {"source_pts_end", metadata.source_pts_end
                    ? json(*metadata.source_pts_end)
                    : json(nullptr)},
            {"wall_clock_start_ms", metadata.wall_clock_start_ms
                    ? json(*metadata.wall_clock_start_ms)
                    : json(nullptr)},
            {"wall_clock_end_ms", metadata.wall_clock_end_ms
                    ? json(*metadata.wall_clock_end_ms)
                    : json(nullptr)},
            {"media", media_path.filename().string()},
            {"finalized", true},
        };
        publish_sidecar(
            media_path.replace_extension(".json"), sidecar.dump(2) + "\n");
        cleanup_retained_fragments();
    }

    void publish_sidecar(const fs::path &sidecar_path, std::string contents)
    {
        const auto temporary = sidecar_path.string() + ".tmp";
        const int descriptor = ::open(
            temporary.c_str(),
            O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC,
            0640);
        if (descriptor < 0)
            throw std::system_error(
                errno,
                std::generic_category(),
                "failed to open evidence sidecar temporary file");

        bool complete = false;
        try {
            if (!write_all(descriptor, contents) || ::fsync(descriptor) != 0)
                throw std::system_error(
                    errno,
                    std::generic_category(),
                    "failed to write evidence sidecar");
            if (::close(descriptor) != 0)
                throw std::system_error(
                    errno,
                    std::generic_category(),
                    "failed to close evidence sidecar");
            if (::rename(temporary.c_str(), sidecar_path.c_str()) != 0)
                throw std::system_error(
                    errno,
                    std::generic_category(),
                    "failed to publish evidence sidecar");
            fsync_directory(source_directory_);
            complete = true;
        } catch (...) {
            ::close(descriptor);
            if (!complete)
                ::unlink(temporary.c_str());
            throw;
        }
    }

    struct RetainedFragment {
        fs::path media;
        fs::path sidecar;
        std::uintmax_t bytes = 0;
        std::optional<std::int64_t> wall_clock_end_ms;
    };

    std::vector<RetainedFragment> retained_fragments() const
    {
        std::vector<RetainedFragment> fragments;
        std::error_code error;
        for (const auto &entry : fs::directory_iterator(source_directory_, error)) {
            if (error)
                break;
            if (entry.path().extension() != ".json")
                continue;
            try {
                const auto sidecar_path = entry.path();
                const auto sidecar_status = fs::symlink_status(sidecar_path);
                if (sidecar_status.type() != fs::file_type::regular)
                    continue;
                const auto media_name = sidecar_path.stem().string() + ".mp4";
                if (!is_fragment_name(media_name))
                    continue;
                const auto media_path = source_directory_ / media_name;
                const auto status = fs::symlink_status(media_path);
                if (status.type() != fs::file_type::regular)
                    continue;
                std::ifstream input(sidecar_path);
                const auto sidecar = json::parse(input);
                if (sidecar.value("schema", "") != kSidecarSchema
                    || !sidecar.value("finalized", false)
                    || sidecar.value("media", "") != media_name) {
                    continue;
                }
                std::optional<std::int64_t> wall_clock_end_ms;
                if (sidecar.contains("wall_clock_end_ms")
                    && !sidecar["wall_clock_end_ms"].is_null()) {
                    wall_clock_end_ms =
                        sidecar["wall_clock_end_ms"].get<std::int64_t>();
                }
                fragments.push_back({
                    media_path,
                    sidecar_path,
                    fs::file_size(media_path) + fs::file_size(sidecar_path),
                    wall_clock_end_ms,
                });
            } catch (...) {
                continue;
            }
        }
        if (error)
            throw std::system_error(
                error.value(), error.category(),
                "failed to inspect evidence cache retention directory");
        return fragments;
    }

    void remove_fragment(const RetainedFragment &fragment)
    {
        std::error_code error;
        fs::remove(fragment.media, error);
        if (error)
            throw std::system_error(
                error.value(), error.category(),
                "failed to remove expired evidence media");
        fs::remove(fragment.sidecar, error);
        if (error)
            throw std::system_error(
                error.value(), error.category(),
                "failed to remove expired evidence sidecar");
    }

    void cleanup_retained_fragments()
    {
        auto fragments = retained_fragments();
        const auto now = system_now_ms();
        const auto retention_ms = static_cast<std::int64_t>(config_.retention_ms);
        for (const auto &fragment : fragments) {
            if (!fragment.wall_clock_end_ms
                || now - *fragment.wall_clock_end_ms < retention_ms)
                continue;
            remove_fragment(fragment);
        }

        fragments = retained_fragments();
        std::uintmax_t total_bytes = 0;
        for (const auto &fragment : fragments)
            total_bytes += fragment.bytes;
        const auto max_bytes = static_cast<std::uintmax_t>(config_.max_bytes_mb)
            * 1024U * 1024U;
        if (total_bytes <= max_bytes)
            return;

        std::sort(
            fragments.begin(), fragments.end(),
            [](const auto &left, const auto &right) {
                return left.wall_clock_end_ms < right.wall_clock_end_ms;
            });
        for (const auto &fragment : fragments) {
            if (total_bytes <= max_bytes)
                break;
            remove_fragment(fragment);
            total_bytes = fragment.bytes >= total_bytes
                ? 0
                : total_bytes - fragment.bytes;
        }
    }

    void mark_unavailable(std::string_view reason) noexcept
    {
        if (unavailable_.exchange(true))
            return;
        g_warning(
            "SSV evidence cache for source '%s' is unavailable: %.*s",
            source_id_.c_str(),
            static_cast<int>(reason.size()),
            reason.data());
    }

    SsvEvidenceCacheConfig config_;
    std::string source_id_;
    std::shared_ptr<SsvSourceContext> source_context_;
    fs::path source_directory_;
    std::string location_pattern_;
    GstElement *splitmux_sink_ = nullptr;
    GstPad *parser_src_pad_ = nullptr;
    gulong probe_id_ = 0;
    std::unique_ptr<SsvTimelineCursor> timeline_;
    mutable std::mutex observation_mutex_;
    GstSegment segment_;
    bool has_segment_ = false;
    std::deque<ObservedBuffer> observations_;
    std::unordered_map<std::string, GstClockTime> fragment_starts_;
    std::atomic_bool configured_ = false;
    std::atomic_bool unavailable_ = false;
};

SsvEvidenceCache::SsvEvidenceCache(
    SsvEvidenceCacheConfig config,
    std::string source_id,
    std::shared_ptr<SsvSourceContext> source_context)
    : impl_(std::make_unique<Impl>(
          std::move(config), std::move(source_id), std::move(source_context)))
{
}

SsvEvidenceCache::~SsvEvidenceCache() = default;

void SsvEvidenceCache::configure(
    GstElement *parser,
    GstElement *splitmux_sink)
{
    impl_->configure(parser, splitmux_sink);
}

void SsvEvidenceCache::handle_message(GstMessage *message) noexcept
{
    impl_->handle_message(message);
}

} // namespace ssv
