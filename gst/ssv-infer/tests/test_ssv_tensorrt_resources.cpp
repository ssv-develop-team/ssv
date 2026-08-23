#include "backends/tensorrt/ssv_tensorrt_resources.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
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

struct HardwareCallState {
    int active_device = -1;
    std::size_t allocations = 0;
    std::size_t event_allocations = 0;
    std::vector<std::string> calls;
};

class HardwareCountingCudaApi final : public ssv::infer::SsvTensorRtCudaApi {
public:
    explicit HardwareCountingCudaApi(std::shared_ptr<HardwareCallState> state)
        : state_(std::move(state))
    {
    }

    ssv::infer::SsvTensorRtCudaDeviceInfo select_device(int device_id) override
    {
        state_->active_device = device_id;
        state_->calls.emplace_back("select");
        return {};
    }

    void activate_device(int device_id) override
    {
        state_->active_device = device_id;
        state_->calls.emplace_back("activate");
    }

    void *create_stream() override
    {
        state_->calls.emplace_back("stream_create");
        return reinterpret_cast<void *>(1U);
    }

    void destroy_stream(void *) noexcept override
    {
        state_->calls.emplace_back("stream_destroy");
    }

    void *allocate_device(std::size_t bytes) override
    {
        state_->calls.emplace_back("allocate:" + std::to_string(bytes));
        ++state_->allocations;
        return reinterpret_cast<void *>(100U + state_->allocations);
    }

    void free_device(void *device) noexcept override
    {
        state_->calls.emplace_back(
            "free:" + std::to_string(reinterpret_cast<std::uintptr_t>(device)));
    }

    void copy_host_to_device(
        void *, const void *, std::size_t, void *) override
    {
        assert(false && "hardware test must use the 2D copy path");
    }

    void copy_host_to_device_2d(
        void *device,
        std::size_t device_pitch,
        const void *,
        std::size_t host_pitch,
        std::size_t width_bytes,
        std::size_t height,
        void *) override
    {
        assert(device == reinterpret_cast<void *>(102U));
        assert(device_pitch == 8);
        assert(host_pitch == 12);
        assert(width_bytes == 8);
        assert(height == 1);
        state_->calls.emplace_back("copy_2d");
    }

    void copy_device_to_host(
        void *, const void *device, std::size_t, void *) override
    {
        assert(device == reinterpret_cast<void *>(101U));
        state_->calls.emplace_back("copy_d2h");
    }

    void synchronize(void *) override
    {
        state_->calls.emplace_back("synchronize");
    }

    [[nodiscard]] bool supports_hardware_preprocess() const noexcept override
    {
        return true;
    }

    void launch_rgba_to_float_nchw(
        void *output,
        const void *rgba,
        std::size_t stride,
        const ssv::infer::SsvPreprocessPlan &plan,
        void *) override
    {
        assert(output == reinterpret_cast<void *>(101U));
        assert(rgba == reinterpret_cast<void *>(102U));
        assert(stride == 8);
        assert(plan.canvas_width == 2);
        assert(plan.canvas_height == 1);
        state_->calls.emplace_back("launch");
    }

    [[nodiscard]] bool timing_supported() const noexcept override
    {
        return true;
    }

    [[nodiscard]] void *create_event() override
    {
        ++state_->event_allocations;
        return reinterpret_cast<void *>(200U + state_->event_allocations);
    }

    void destroy_event(void *event) noexcept override
    {
        state_->calls.emplace_back(
            "event_destroy:"
            + std::to_string(reinterpret_cast<std::uintptr_t>(event)));
    }

    void record_event(void *event, void *) override
    {
        state_->calls.emplace_back(
            "event_record:"
            + std::to_string(reinterpret_cast<std::uintptr_t>(event)));
    }

    [[nodiscard]] float elapsed_event_ms(void *, void *) override
    {
        return 1.5F;
    }

private:
    std::shared_ptr<HardwareCallState> state_;
};

void test_hardware_preprocess_reuses_scratch_and_reports_stages()
{
    auto state = std::make_shared<HardwareCallState>();
    {
        ssv::infer::SsvTensorRtExecutionResources resources(
            std::make_unique<HardwareCountingCudaApi>(state));
        static_cast<void>(resources.start(kSelectedDeviceId));
        const std::array<std::size_t, 1> buffer_sizes{24};
        resources.allocate(buffer_sizes);
        assert(resources.prepare_hardware_preprocess(2, 1));
        assert(resources.prepare_hardware_preprocess(2, 1));
        assert(state->allocations == 2);

        ssv::infer::SsvPreprocessPlan plan;
        plan.input.name = "images";
        plan.input.dtype = ssv::infer::DataType::Float32;
        plan.input.layout = ssv::infer::TensorLayout::Nchw;
        plan.input.shape = {1, 3, 1, 2};
        plan.canvas_width = 2;
        plan.canvas_height = 1;
        plan.color_order = ssv::SsvInputColorOrder::Bgr;
        std::array<std::uint8_t, 12> rgba_bytes{};
        const ::SsvRgbaFrameView rgba {
            rgba_bytes, 2, 1, 12};

        resources.begin_run_timing();
        resources.preprocess_rgba_to_float_nchw(0, rgba, plan);
        resources.mark_execution_start();
        resources.mark_execution_end();
        std::array<std::byte, 24> output{};
        resources.copy_output(0, output);
        resources.mark_output_end();
        resources.synchronize();
        const auto timings = resources.finish_run_timing();
        assert(timings.h2d_us && *timings.h2d_us == 1500);
        assert(timings.preprocess_us && *timings.preprocess_us == 1500);
        assert(timings.execution_us && *timings.execution_us == 1500);
        assert(timings.d2h_us && *timings.d2h_us == 1500);
    }

    assert(state->calls.size() >= 4);
    const auto free_scratch = std::find(
        state->calls.begin(), state->calls.end(), "free:102");
    const auto free_binding = std::find(
        state->calls.begin(), state->calls.end(), "free:101");
    const auto stream_destroy = std::find(
        state->calls.begin(), state->calls.end(), "stream_destroy");
    assert(free_scratch != state->calls.end());
    assert(free_binding != state->calls.end());
    assert(stream_destroy != state->calls.end());
    assert(free_scratch < free_binding);
    assert(free_binding < stream_destroy);
}

} // namespace

int main()
{
    test_reuses_stream_and_fixed_device_buffers();
    test_partial_allocation_failure_releases_started_resources();
    test_hardware_preprocess_reuses_scratch_and_reports_stages();
    return 0;
}
