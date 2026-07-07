#pragma once

#include "runtime.hpp"

namespace ssv::provider {

class OnnxCpuRuntime final : public Runtime {
public:
    bool configure(
        Ort::SessionOptions& options,
        RuntimeStatus* status,
        std::string* error) const override;
};

} // namespace ssv::provider
