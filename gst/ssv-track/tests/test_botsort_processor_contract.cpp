#include "adapters/ssv_track_adapter.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

SsvDetection make_detection(float x1, float y1, float x2, float y2)
{
    SsvDetection detection;
    std::snprintf(detection.class_name, sizeof(detection.class_name), "person");
    detection.confidence = 0.9F;
    detection.x1 = x1;
    detection.y1 = y1;
    detection.x2 = x2;
    detection.y2 = y2;
    detection.class_id = 0;
    return detection;
}

std::vector<SsvDetection> one_detection(SsvDetection detection)
{
    std::vector<SsvDetection> detections;
    detections.push_back(std::move(detection));
    return detections;
}

void test_gmc_none_tracks_without_an_rgba_view()
{
    botsort::TrackerConfig config;
    config.gmc_method = botsort::GmcMethod::kNone;
    botsort::SsvTrackAdapter processor(config);

    const auto detection = make_detection(0.1F, 0.2F, 0.3F, 0.5F);

    const PreprocessTransform transform {
        640, 480, 640, 640, 1.0F, 0, 80, 0, 80,
    };
    auto tracked = processor.process(
        one_detection(detection), transform);
    assert(tracked.size() == 1);
    assert(tracked.front().detection.class_id == 0);
    assert(tracked.front().detection.x1 == detection.x1);
    assert(tracked.front().track_id >= 0);
    assert(tracked.front().track_state == SSV_TRACK_NEW);
    assert(!tracked.front().occluded);
}

void test_resolution_change_resets_tracker_state()
{
    botsort::TrackerConfig config;
    config.gmc_method = botsort::GmcMethod::kNone;
    botsort::SsvTrackAdapter processor(config);
    const PreprocessTransform first_geometry {
        640, 480, 640, 640, 1.0F, 0, 80, 0, 80,
    };

    auto first = processor.process(
        one_detection(make_detection(0.05F, 0.1F, 0.15F, 0.3F)),
        first_geometry);
    auto second = processor.process(
        one_detection(make_detection(0.8F, 0.1F, 0.9F, 0.3F)),
        first_geometry);
    assert(first.size() == 1 && first.front().track_id == 1);
    assert(second.size() == 1 && second.front().track_id == 2);

    const PreprocessTransform changed_geometry {
        1280, 720, 640, 640, 0.5F, 0, 140, 0, 140,
    };
    auto after_change = processor.process(
        one_detection(make_detection(0.4F, 0.1F, 0.5F, 0.3F)),
        changed_geometry);

    assert(after_change.size() == 1);
    assert(after_change.front().track_id == 1);
    assert(after_change.front().track_state == SSV_TRACK_NEW);
}

void test_explicit_reset_restarts_track_identity()
{
    botsort::TrackerConfig config;
    config.gmc_method = botsort::GmcMethod::kNone;
    botsort::SsvTrackAdapter adapter(config);
    const PreprocessTransform transform {
        640, 480, 640, 640, 1.0F, 0, 80, 0, 80,
    };

    const auto before = adapter.process(
        one_detection(make_detection(0.1F, 0.2F, 0.3F, 0.5F)),
        transform,
        std::nullopt);
    assert(before.size() == 1 && before.front().track_id == 1);

    adapter.reset();
    const auto after = adapter.process(
        one_detection(make_detection(0.6F, 0.1F, 0.8F, 0.3F)), transform);
    assert(after.size() == 1 && after.front().track_id == 1);
}

void test_invalid_source_geometry_is_rejected()
{
    botsort::TrackerConfig config;
    config.gmc_method = botsort::GmcMethod::kNone;
    botsort::SsvTrackAdapter adapter(config);

    bool threw = false;
    try {
        static_cast<void>(adapter.process(
            one_detection(make_detection(0.1F, 0.2F, 0.3F, 0.5F)),
            PreprocessTransform {0, 480, 640, 640, 1.0F, 0, 80, 0, 80}));
    } catch (const std::invalid_argument &error) {
        threw = true;
        assert(std::string(error.what())
            == "tracker source dimensions must be positive");
    }
    assert(threw);
}

void test_model_warp_is_conjugated_into_source_coordinates()
{
    botsort::GmcWarp model_warp;
    model_warp.m00 = 1.0;
    model_warp.m01 = 0.1;
    model_warp.m02 = 4.0;
    model_warp.m10 = -0.2;
    model_warp.m11 = 1.0;
    model_warp.m12 = 6.0;

    const auto source_warp = botsort::gmc_warp_to_source_coordinates(
        model_warp, 2.0F, 10, 20);

    assert(std::fabs(source_warp.m00 - 1.0) < 0.0001);
    assert(std::fabs(source_warp.m01 - 0.1) < 0.0001);
    assert(std::fabs(source_warp.m02 - 3.0) < 0.0001);
    assert(std::fabs(source_warp.m10 + 0.2) < 0.0001);
    assert(std::fabs(source_warp.m11 - 1.0) < 0.0001);
    assert(std::fabs(source_warp.m12 - 2.0) < 0.0001);
}

void test_gmc_method_availability_matches_build()
{
    assert(botsort::gmc_method_available(botsort::GmcMethod::kNone));
    assert(
        botsort::gmc_method_available(botsort::GmcMethod::kSparseOptFlow)
        == static_cast<bool>(SSV_HAS_OPENCV));
}

#if SSV_HAS_OPENCV
void test_sparse_gmc_estimates_rgba_translation()
{
    constexpr int width = 96;
    constexpr int height = 72;
    constexpr int shift_x = 6;
    constexpr int shift_y = 3;
    std::vector<std::uint8_t> first(width * height * 4, 0);
    std::vector<std::uint8_t> second(width * height * 4, 0);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto value = static_cast<std::uint8_t>(
                (x * 37 + y * 17 + (x * y) % 251) % 256);
            const auto offset = static_cast<std::size_t>((y * width + x) * 4);
            first[offset + 0] = value;
            first[offset + 1] = value;
            first[offset + 2] = value;
            first[offset + 3] = 255;
            second[offset + 3] = 255;
        }
    }
    for (int y = 0; y < height - shift_y; ++y) {
        for (int x = 0; x < width - shift_x; ++x) {
            const auto source = static_cast<std::size_t>((y * width + x) * 4);
            const auto destination = static_cast<std::size_t>(
                (((y + shift_y) * width + x + shift_x) * 4));
            for (int channel = 0; channel < 4; ++channel)
                second[destination + channel] = first[source + channel];
        }
    }

    botsort::GmcFrameView first_view {
        first, width, height, static_cast<std::size_t>(width * 4), 1.0F, 0, 0,
    };
    botsort::GmcFrameView second_view {
        second, width, height, static_cast<std::size_t>(width * 4), 1.0F, 0, 0,
    };
    botsort::BoTSortGmc gmc(botsort::GmcMethod::kSparseOptFlow, 1);
    static_cast<void>(gmc.estimate(&first_view));
    const auto warp = gmc.estimate(&second_view);

    assert(std::fabs(warp.m02 - shift_x) < 1.0);
    assert(std::fabs(warp.m12 - shift_y) < 1.0);
}
#else
void test_sparse_gmc_without_opencv_fails_strictly()
{
    std::vector<std::uint8_t> rgba(4 * 4 * 4, 0);
    const botsort::GmcFrameView frame {
        rgba, 4, 4, 16, 1.0F, 0, 0,
    };
    botsort::BoTSortGmc gmc(botsort::GmcMethod::kSparseOptFlow, 1);

    bool threw = false;
    try {
        static_cast<void>(gmc.estimate(&frame));
    } catch (const std::runtime_error &) {
        threw = true;
    }
    assert(threw);
}

void test_sparse_gmc_without_opencv_rejects_missing_frame()
{
    botsort::BoTSortGmc gmc(botsort::GmcMethod::kSparseOptFlow, 1);

    bool threw = false;
    try {
        static_cast<void>(gmc.estimate(nullptr));
    } catch (const std::runtime_error &error) {
        threw = true;
        assert(std::string(error.what())
            == "sparse-opt-flow GMC requires OpenCV support");
    }
    assert(threw);
}
#endif

} // namespace

int main()
{
    test_gmc_none_tracks_without_an_rgba_view();
    test_resolution_change_resets_tracker_state();
    test_explicit_reset_restarts_track_identity();
    test_invalid_source_geometry_is_rejected();
    test_model_warp_is_conjugated_into_source_coordinates();
    test_gmc_method_availability_matches_build();
#if SSV_HAS_OPENCV
    test_sparse_gmc_estimates_rgba_translation();
#else
    test_sparse_gmc_without_opencv_fails_strictly();
    test_sparse_gmc_without_opencv_rejects_missing_frame();
#endif
    return 0;
}
