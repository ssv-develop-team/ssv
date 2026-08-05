#pragma once

#include "ssv_pipeline_contract.hpp"

#include <gst/gst.h>

#include <functional>

namespace ssv::pipeline_internal {

void watch_contract(
    GstElement *error_source,
    GstPad *pad,
    SsvPipelineContractExpectation expectation,
    std::function<void(int width, int height)> on_geometry = {});

} // namespace ssv::pipeline_internal
