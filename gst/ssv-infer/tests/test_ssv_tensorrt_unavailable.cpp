#include "core/ssv_inference_backend.hpp"

#include <cassert>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <variant>

namespace {

class CountingAllocator final : public ssv::infer::SsvInferenceBufferAllocator {
public:
    ssv::infer::SsvInferenceBuffer allocate(std::size_t, std::size_t) override
    {
        ++allocation_count;
        throw std::logic_error("unavailable backend requested a host buffer");
    }

    std::size_t allocation_count = 0;
};

} // namespace

int main()
{
    auto backend = ssv::infer::create_tensorrt_backend();
    assert(backend != nullptr);
    assert(std::holds_alternative<ssv::infer::TensorRtEngineBackendInfo>(
        backend->info().runtime));

    ssv::infer::InferenceConfig config;
    config.runtime = ssv::infer::RuntimeKind::TensorRtEngine;
    CountingAllocator allocator;
    try {
        static_cast<void>(backend->load(config, allocator));
        assert(false && "unavailable TensorRT backend loaded successfully");
    } catch (const std::runtime_error &error) {
        const std::string message = error.what();
        assert(
            message.find("TensorRT backend is not built") != std::string::npos);
        assert(message.find("SSV_TENSORRT_MODE=enabled") != std::string::npos);
    }
    assert(allocator.allocation_count == 0);
    return 0;
}
