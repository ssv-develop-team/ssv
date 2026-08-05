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
