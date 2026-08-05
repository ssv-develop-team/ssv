#include "ssv_hardware_capabilities.hpp"

#include <gst/gst.h>

#include <algorithm>
#include <memory>
#include <stdexcept>

namespace ssv {
bool SsvHardwareCapabilities::has_gstreamer_element(
    std::string_view factory_name) const
{
    return std::ranges::find(gstreamer_elements, factory_name)
        != gstreamer_elements.end();
}

SsvHardwareCapabilities SsvSystemHardwareCapabilitiesProbe::detect() const
{
    if (!gst_is_initialized()) {
        throw std::logic_error(
            "GStreamer must be initialized before capability detection");
    }

    SsvHardwareCapabilities capabilities;
    std::unique_ptr<GList, decltype(&gst_plugin_feature_list_free)> features(
        gst_registry_get_feature_list(
            gst_registry_get(), GST_TYPE_ELEMENT_FACTORY),
        gst_plugin_feature_list_free);
    for (GList *entry = features.get(); entry != nullptr;
         entry = entry->next) {
        const auto *feature = GST_PLUGIN_FEATURE(entry->data);
        capabilities.gstreamer_elements.emplace_back(
            gst_plugin_feature_get_name(feature));
    }
    std::ranges::sort(capabilities.gstreamer_elements);
    capabilities.gstreamer_elements.erase(
        std::ranges::unique(capabilities.gstreamer_elements).begin(),
        capabilities.gstreamer_elements.end());
    capabilities.onnxruntime_available = SSV_HAS_ONNXRUNTIME != 0;
    capabilities.tensorrt_engine_available =
        SSV_HAS_TENSORRT_ENGINE != 0;
    return capabilities;
}

} // namespace ssv
