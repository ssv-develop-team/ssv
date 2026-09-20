#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace ssv {

struct SsvHardwareCapabilities {
    std::vector<std::string> gstreamer_elements;
    bool onnxruntime_available = false;
    bool tensorrt_engine_available = false;

    [[nodiscard]] bool has_gstreamer_element(
        std::string_view factory_name) const;
};

class SsvCapabilitySnapshot final {
public:
    SsvCapabilitySnapshot() = default;
    explicit SsvCapabilitySnapshot(SsvHardwareCapabilities capabilities);

    [[nodiscard]] bool has_gstreamer_element(
        std::string_view factory_name) const;
    [[nodiscard]] bool onnxruntime_available() const noexcept;
    [[nodiscard]] bool tensorrt_engine_available() const noexcept;

private:
    SsvHardwareCapabilities capabilities_;
};

class SsvHardwareCapabilitiesProbe {
public:
    virtual ~SsvHardwareCapabilitiesProbe() = default;

    [[nodiscard]] virtual SsvHardwareCapabilities detect() const = 0;
};

class SsvSystemHardwareCapabilitiesProbe final
    : public SsvHardwareCapabilitiesProbe {
public:
    [[nodiscard]] SsvHardwareCapabilities detect() const override;
};

} // namespace ssv
