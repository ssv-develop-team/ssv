#pragma once

#include <cstddef>
#include <string>

namespace ssv::infer {

struct SsvModelContract {
    int width;
    int height;
    std::size_t input_bytes;
    std::string contract;
    std::string source_sha256;
};

} // namespace ssv::infer
