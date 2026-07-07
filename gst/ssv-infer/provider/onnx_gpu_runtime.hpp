#pragma once

#include "runtime.hpp"

namespace ssv::provider {

bool read_cuda_device_id_from_env(int* device_id, std::string* error);

class OnnxGpuRuntime final : public Runtime {
public:
    bool configure(
        Ort::SessionOptions& options,
        RuntimeStatus* status,
        std::string* error) const override;
};

} // namespace ssv::provider
