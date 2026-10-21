#include "pipeline/ssv_evidence_cache.hpp"

#include <gst/gst.h>
#include <nlohmann/json.hpp>

#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

class TemporaryDirectory final {
public:
    TemporaryDirectory()
    {
        char path[] = "/tmp/ssv-evidence-cache-test-XXXXXX";
        const char *created = mkdtemp(path);
        assert(created != nullptr);
        path_ = created;
    }

    ~TemporaryDirectory()
    {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    [[nodiscard]] const fs::path &path() const noexcept { return path_; }

private:
    fs::path path_;
};

GstPadProbeReturn reorder_encoded_timestamps(
    GstPad *, GstPadProbeInfo *info, gpointer user_data)
{
    auto &frame_index = *static_cast<guint64 *>(user_data);
    auto *buffer = gst_buffer_make_writable(GST_PAD_PROBE_INFO_BUFFER(info));
    GST_PAD_PROBE_INFO_DATA(info) = buffer;
    const auto within_gop = frame_index % 10;
    const auto reordered = within_gop < 2
        ? within_gop
        : (within_gop % 2 == 0 ? within_gop + 1 : within_gop - 1);
    GST_BUFFER_DTS(buffer) = frame_index * (GST_SECOND / 10);
    GST_BUFFER_PTS(buffer) = (frame_index - within_gop + reordered)
        * (GST_SECOND / 10);
    ++frame_index;
    return GST_PAD_PROBE_OK;
}

enum class CacheInput {
    Generated,
    ReorderedPts,
    RoundedDuration,
    BFramesWithoutDts,
};

std::string capture_command(std::vector<std::string> arguments)
{
    std::vector<gchar *> argv;
    for (auto &argument : arguments)
        argv.push_back(argument.data());
    argv.push_back(nullptr);
    gchar *output = nullptr;
    gchar *errors = nullptr;
    gint status = -1;
    GError *error = nullptr;
    const auto spawned = g_spawn_sync(
        nullptr, argv.data(), nullptr, G_SPAWN_SEARCH_PATH,
        nullptr, nullptr, &output, &errors, &status, &error);
    if (!spawned || status != 0)
        g_printerr("evidence timing fixture failed: %s\n",
            error != nullptr ? error->message : (errors != nullptr ? errors : ""));
    assert(spawned && status == 0);
    const std::string captured = output != nullptr ? output : "";
    g_free(output);
    g_free(errors);
    g_clear_error(&error);
    return captured;
}

void test_finalized_sidecar_contains_source_timing(
    CacheInput input = CacheInput::Generated)
{
    TemporaryDirectory temporary;
    const bool b_frames = input == CacheInput::BFramesWithoutDts;

    if (!b_frames && gst_element_factory_find("openh264enc") == nullptr) {
        // The production pipeline can use another H.264 encoder. This test only
        // needs one encoder to exercise splitmuxsink with real media.
        return;
    }

    std::string source =
        "videotestsrc num-buffers=40 is-live=false ! "
        "video/x-raw,format=I420,width=64,height=48,framerate=10/1 ! "
        "openh264enc gop-size=10 ! ";
    if (input == CacheInput::RoundedDuration) {
        source =
            "videotestsrc num-buffers=120 is-live=false ! "
            "video/x-raw,format=I420,width=64,height=48,framerate=30/1 ! "
            "openh264enc gop-size=30 ! ";
    }
    if (b_frames) {
        auto *ffmpeg = g_find_program_in_path("ffmpeg");
        auto *ffprobe = g_find_program_in_path("ffprobe");
        const bool available = ffmpeg != nullptr && ffprobe != nullptr;
        g_free(ffmpeg);
        g_free(ffprobe);
        if (!available) {
            g_printerr("B-frame evidence timing test requires ffmpeg and ffprobe\n");
            return;
        }
        const auto fixture = temporary.path() / "b-frames.mp4";
        capture_command({
            "ffmpeg", "-hide_banner", "-loglevel", "error",
            "-f", "lavfi", "-i", "testsrc2=size=64x48:rate=10",
            "-frames:v", "40", "-an", "-c:v", "libx264", "-bf", "2",
            "-g", "10", "-sc_threshold", "0", "-threads", "1",
            fixture.string(),
        });
        source = "filesrc location=\"" + fixture.string()
            + "\" ! qtdemux ! ";
    }
    const auto description = source
        + "h264parse name=h264-parser ! tee name=encoded-tee "
          "encoded-tee. ! queue ! h264timestamper ! "
          "splitmuxsink name=evidence-cache-sink";
    GError *parse_error = nullptr;
    GstElement *pipeline = gst_parse_launch(description.c_str(), &parse_error);
    assert(pipeline != nullptr && parse_error == nullptr);
    if (parse_error != nullptr)
        g_error_free(parse_error);

    auto *parser = gst_bin_get_by_name(GST_BIN(pipeline), "h264-parser");
    auto *splitmux = gst_bin_get_by_name(
        GST_BIN(pipeline), "evidence-cache-sink");
    assert(parser != nullptr && splitmux != nullptr);

    guint64 frame_index = 0;
    if (input == CacheInput::ReorderedPts) {
        auto *pad = gst_element_get_static_pad(parser, "src");
        assert(pad != nullptr);
        gst_pad_add_probe(
            pad, GST_PAD_PROBE_TYPE_BUFFER,
            reorder_encoded_timestamps, &frame_index, nullptr);
        gst_object_unref(pad);
    }
    if (input == CacheInput::RoundedDuration) {
        auto *pad = gst_element_get_static_pad(parser, "src");
        assert(pad != nullptr);
        gst_pad_add_probe(
            pad, GST_PAD_PROBE_TYPE_BUFFER,
            [](GstPad *, GstPadProbeInfo *info, gpointer data) {
                auto &index = *static_cast<guint64 *>(data);
                auto *buffer = gst_buffer_make_writable(
                    GST_PAD_PROBE_INFO_BUFFER(info));
                GST_PAD_PROBE_INFO_DATA(info) = buffer;
                GST_BUFFER_PTS(buffer) = index * GST_SECOND / 30;
                GST_BUFFER_DTS(buffer) = GST_BUFFER_PTS(buffer);
                GST_BUFFER_DURATION(buffer) = GST_SECOND / 30;
                ++index;
                return GST_PAD_PROBE_OK;
            },
            &frame_index, nullptr);
        gst_object_unref(pad);
    }
    if (b_frames) {
        auto *pad = gst_element_get_static_pad(parser, "src");
        assert(pad != nullptr);
        gst_pad_add_probe(
            pad, GST_PAD_PROBE_TYPE_BUFFER,
            [](GstPad *, GstPadProbeInfo *info, gpointer) {
                auto *buffer = gst_buffer_make_writable(
                    GST_PAD_PROBE_INFO_BUFFER(info));
                GST_PAD_PROBE_INFO_DATA(info) = buffer;
                GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
                return GST_PAD_PROBE_OK;
            },
            nullptr, nullptr);
        gst_object_unref(pad);
    }

    auto source_context = std::make_shared<SsvSourceContext>("camera-01");
    ssv::SsvEvidenceCacheConfig config;
    config.directory = temporary.path().string();
    config.segment_duration_ms = 1000;
    config.retention_ms = 10'000;
    config.max_bytes_mb = 1;
    {
        ssv::SsvEvidenceCache cache(config, "camera-01", source_context);
        cache.configure(parser, splitmux);
        gst_object_unref(parser);

        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        GstState state = GST_STATE_NULL;
        assert(gst_element_get_state(
            pipeline, &state, nullptr, 5 * GST_SECOND)
            != GST_STATE_CHANGE_FAILURE);

        GstBus *bus = gst_element_get_bus(pipeline);
        assert(bus != nullptr);
        bool reached_eos = false;
        while (!reached_eos) {
            auto *message = gst_bus_timed_pop_filtered(
                bus,
                5 * GST_SECOND,
                static_cast<GstMessageType>(
                    GST_MESSAGE_ELEMENT | GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
            assert(message != nullptr);
            if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ELEMENT)
                cache.handle_message(message);
            else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
                GError *error = nullptr;
                gchar *debug = nullptr;
                gst_message_parse_error(message, &error, &debug);
                if (error != nullptr)
                    g_printerr("evidence cache pipeline error: %s\n", error->message);
                g_clear_error(&error);
                g_free(debug);
                assert(false);
            } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
                reached_eos = true;
            }
            gst_message_unref(message);
        }
        gst_object_unref(bus);

        const auto media = temporary.path()
            / "camera-01" / "segment-00000000.mp4";
        const auto sidecar_path = media.parent_path()
            / "segment-00000000.json";
        assert(fs::is_regular_file(media));
        assert(fs::is_regular_file(sidecar_path));
        std::ifstream input(sidecar_path);
        const auto sidecar = json::parse(input);
        assert(sidecar.at("schema") == "ssv.evidence-segment.v1");
        assert(sidecar.at("source") == "camera-01");
        assert(sidecar.at("stream_generation").is_number_unsigned());
        assert(sidecar.at("source_pts_start").is_number_unsigned());
        assert(sidecar.at("source_pts_end").is_number_unsigned());
        assert(
            sidecar.at("source_pts_end").get<guint64>()
            > sidecar.at("source_pts_start").get<guint64>());
        assert(sidecar.at("media") == "segment-00000000.mp4");
        assert(sidecar.at("finalized"));
        if (!b_frames)
            assert(sidecar.at("source_pts_start") == 0);
        const auto first_pts = sidecar.at("source_pts_start").get<guint64>();
        assert(sidecar.at("source_pts_end") == first_pts + GST_SECOND);
        if (b_frames) {
            const auto probe = json::parse(capture_command({
                "ffprobe", "-v", "error", "-show_entries", "format=duration",
                "-of", "json", media.string(),
            }));
            const auto duration = std::stod(
                probe.at("format").at("duration").get<std::string>());
            assert(std::abs(duration - 1.0) < 0.001);
        }

        const auto second_media = temporary.path()
            / "camera-01" / "segment-00000001.mp4";
        const auto second_sidecar_path = second_media.parent_path()
            / "segment-00000001.json";
        assert(fs::is_regular_file(second_media));
        assert(fs::is_regular_file(second_sidecar_path));
        std::ifstream second_input(second_sidecar_path);
        const auto second_sidecar = json::parse(second_input);
        assert(second_sidecar.at("stream_generation")
            == sidecar.at("stream_generation"));
        assert(second_sidecar.at("source_pts_start") == first_pts + GST_SECOND);
        assert(second_sidecar.at("source_pts_end") == first_pts + 2 * GST_SECOND);

        const auto media_with_symlink_sidecar = temporary.path()
            / "camera-01" / "segment-00000004.mp4";
        {
            std::ofstream output(media_with_symlink_sidecar, std::ios::binary);
            output << std::string(2 * 1024 * 1024, 'x');
        }
        const auto outside_sidecar = temporary.path()
            / "outside-sidecar.json";
        std::ofstream(outside_sidecar)
            << json({
                   {"schema", "ssv.evidence-segment.v1"},
                   {"source", "camera-01"},
                   {"stream_generation", source_context->meta()->generation()},
                   {"source_pts_start", 0},
                   {"source_pts_end", GST_SECOND},
                   {"wall_clock_end_ms", 0},
                   {"media", "segment-00000001.mp4"},
                   {"finalized", true},
               })
                .dump();
        auto symlink_sidecar = media_with_symlink_sidecar;
        symlink_sidecar.replace_extension(".json");
        fs::create_symlink(outside_sidecar, symlink_sidecar);

        const auto next_media = temporary.path()
            / "camera-01" / "segment-00000005.mp4";
        std::ofstream(next_media) << "next-media";
        auto *next_details = gst_structure_new(
            "splitmuxsink-fragment-closed",
            "fragment-id", G_TYPE_UINT, 5U,
            "location", G_TYPE_STRING, next_media.c_str(),
            "running-time", G_TYPE_UINT64, static_cast<guint64>(GST_SECOND),
            "fragment-offset", G_TYPE_UINT64, static_cast<guint64>(0),
            "fragment-duration", G_TYPE_UINT64, static_cast<guint64>(GST_SECOND),
            nullptr);
        assert(next_details != nullptr);
        auto *next_message = gst_message_new_element(
            GST_OBJECT(splitmux),
            next_details);
        assert(next_message != nullptr);
        cache.handle_message(next_message);
        gst_message_unref(next_message);
        assert(fs::is_regular_file(media_with_symlink_sidecar));
        assert(fs::is_symlink(symlink_sidecar));
        assert(fs::is_regular_file(outside_sidecar));

        gst_element_set_state(pipeline, GST_STATE_NULL);
    }
    gst_object_unref(splitmux);
    gst_object_unref(pipeline);
}

} // namespace

int main(int argc, char **argv)
{
    gst_init(&argc, &argv);
    test_finalized_sidecar_contains_source_timing();
    test_finalized_sidecar_contains_source_timing(CacheInput::ReorderedPts);
    test_finalized_sidecar_contains_source_timing(CacheInput::RoundedDuration);
    test_finalized_sidecar_contains_source_timing(CacheInput::BFramesWithoutDts);
    return 0;
}
