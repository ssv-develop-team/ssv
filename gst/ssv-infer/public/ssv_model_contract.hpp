#pragma once

#include "ssv_config.hpp"

namespace ssv::infer {

struct SsvModelContract {
    int width = 0;
    int height = 0;
    ssv::SsvResizeMode resize_mode = ssv::SsvResizeMode::Letterbox;
};

} // namespace ssv::infer
