#pragma once

#include <onnxruntime_cxx_api.h>

#include <memory>
#include <string>

namespace ssv::provider {

struct RuntimeStatus {
    std::string requested_runtime;
    std::string active_runtime;
};

class Runtime {
public:
    virtual ~Runtime() = default;

    virtual bool configure(
        Ort::SessionOptions& options,
        RuntimeStatus* status,
        std::string* error) const = 0;
};

std::unique_ptr<Runtime> create_runtime(
    const std::string& backend,
    const std::string& device,
    std::string* error);

} // namespace ssv::provider
