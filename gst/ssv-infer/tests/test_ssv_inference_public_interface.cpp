#include "ssv_inference_service.hpp"
#include "ssv_model_contract.hpp"

#include <cassert>
#include <utility>

int main()
{
    ssv::infer::SsvInferenceRequest request;
    request.frame_id = 17;
    request.source_id = "public-interface";
    auto moved_request = std::move(request);

    ssv::infer::SsvInferenceStatsWindow stats;
    stats.completed = 3;
    stats.total.p95_us = 42;
    auto moved_stats = std::move(stats);

    ssv::infer::SsvModelContract contract{
        .width = 640,
        .height = 384,
        .resize_mode = ssv::SsvResizeMode::Letterbox,
    };
    auto moved_contract = std::move(contract);

    assert(moved_request.frame_id == 17);
    assert(moved_request.source_id == "public-interface");
    assert(moved_stats.completed == 3);
    assert(moved_stats.total.p95_us == 42);
    assert(moved_contract.width == 640);
    assert(moved_contract.height == 384);
    assert(moved_contract.resize_mode == ssv::SsvResizeMode::Letterbox);
    return 0;
}
