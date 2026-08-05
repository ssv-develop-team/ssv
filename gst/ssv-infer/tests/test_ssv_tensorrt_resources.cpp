#include "backends/tensorrt/ssv_tensorrt_resources.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr int kSelectedDeviceId = 2;

struct CudaCallState {
    int active_device = -1;
    std::size_t device_activations = 0;
    std::size_t stream_creations = 0;
    std::size_t device_allocations = 0;
    std::size_t host_to_device_copies = 0;
    std::size_t device_to_host_copies = 0;
    std::size_t synchronizations = 0;
    std::vector<std::string> release_events;
};

class CountingCudaApi final : public ssv::infer::SsvTensorRtCudaApi {
public:
    explicit CountingCudaApi(std::shared_ptr<CudaCallState> state,
        std::size_t fail_on_allocation = 0)
        : state_(std::move(state)), fail_on_allocation_(fail_on_allocation)
    {
    }

    ssv::infer::SsvTensorRtCudaDeviceInfo select_device(int device_id) override
    {
        assert(device_id == kSelectedDeviceId);
        state_->active_device = device_id;
        return {
            .cuda_runtime_version = 13020,
            .compute_capability_major = 8,
            .compute_capability_minor = 9,
        };
    }

    void activate_device(int device_id) override
    {
        assert(device_id == kSelectedDeviceId);
        state_->active_device = device_id;
        ++state_->device_activations;
    }

    void *create_stream() override
    {
        ++state_->stream_creations;
        return reinterpret_cast<void *>(1U);
    }

    void destroy_stream(void *stream) noexcept override
    {
        assert(stream == reinterpret_cast<void *>(1U));
        assert(state_->active_device == kSelectedDeviceId);
        state_->release_events.emplace_back("destroy_stream");
    }

    void *allocate_device(std::size_t bytes) override
    {
        assert(bytes == 24);
        assert(state_->active_device == kSelectedDeviceId);
        ++state_->device_allocations;
        if (state_->device_allocations == fail_on_allocation_)
            throw std::runtime_error("injected CUDA allocation failure");
        return reinterpret_cast<void *>(100U + state_->device_allocations);
    }

    void free_device(void *device) noexcept override
    {
        assert(state_->active_device == kSelectedDeviceId);
        state_->release_events.push_back(
            "free:" + std::to_string(reinterpret_cast<std::uintptr_t>(device)));
    }

    void copy_host_to_device(
        void *device, const void *, std::size_t bytes, void *stream) override
    {
        assert(device == reinterpret_cast<void *>(101U));
        assert(bytes == 24);
        assert(stream == reinterpret_cast<void *>(1U));
        ++state_->host_to_device_copies;
    }

    void copy_device_to_host(
        void *, const void *device, std::size_t bytes, void *stream) override
    {
        assert(device == reinterpret_cast<void *>(102U));
        assert(bytes == 24);
        assert(stream == reinterpret_cast<void *>(1U));
        ++state_->device_to_host_copies;
    }

    void synchronize(void *stream) override
    {
        assert(stream == reinterpret_cast<void *>(1U));
        ++state_->synchronizations;
    }

private:
    std::shared_ptr<CudaCallState> state_;
    std::size_t fail_on_allocation_ = 0;
};

void test_reuses_stream_and_fixed_device_buffers()
{
    auto state = std::make_shared<CudaCallState>();
    {
        ssv::infer::SsvTensorRtExecutionResources resources(
            std::make_unique<CountingCudaApi>(state));
        const auto device = resources.start(kSelectedDeviceId);
        assert(device.cuda_runtime_version == 13020);
        const std::array<std::size_t, 2> buffer_sizes{24, 24};
        resources.allocate(buffer_sizes);

        std::array<std::uint8_t, 24> input{};
        std::array<std::byte, 24> output{};
        for (int iteration = 0; iteration < 2; ++iteration) {
            state->active_device = 0;
            assert(resources.activate_stream()
                == reinterpret_cast<void *>(1U));
            resources.copy_input(0, input);
            resources.copy_output(1, output);
            resources.synchronize();
        }

        assert(state->stream_creations == 1);
        assert(state->device_activations == 3);
        assert(state->device_allocations == 2);
        assert(state->host_to_device_copies == 2);
        assert(state->device_to_host_copies == 2);
        assert(state->synchronizations == 2);
        assert(state->release_events.empty());
        state->active_device = 0;
    }

    assert(state->device_activations == 4);

    const std::vector<std::string> expected_release_order{
        "free:102",
        "free:101",
        "destroy_stream",
    };
    assert(state->release_events == expected_release_order);
}

void test_partial_allocation_failure_releases_started_resources()
{
    auto state = std::make_shared<CudaCallState>();
    {
        ssv::infer::SsvTensorRtExecutionResources resources(
            std::make_unique<CountingCudaApi>(state, 2));
        static_cast<void>(resources.start(kSelectedDeviceId));
        state->active_device = 0;
        const std::array<std::size_t, 2> buffer_sizes{24, 24};
        try {
            resources.allocate(buffer_sizes);
            assert(false && "injected CUDA allocation failure was ignored");
        } catch (const std::runtime_error &error) {
            assert(std::string(error.what()).find("injected")
                   != std::string::npos);
        }
    }

    const std::vector<std::string> expected_release_order{
        "free:101",
        "destroy_stream",
    };
    assert(state->device_activations == 2);
    assert(state->release_events == expected_release_order);
}

} // namespace

int main()
{
    test_reuses_stream_and_fixed_device_buffers();
    test_partial_allocation_failure_releases_started_resources();
    return 0;
}
