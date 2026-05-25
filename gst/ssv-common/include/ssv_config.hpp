#pragma once

#include <string>

namespace ssv {

struct SsvConfig {
    // Logging
    std::string cpp_debug_level = "ssv*:4";
    std::string python_log_level = "INFO";

    // Redis
    std::string redis_host = "localhost";
    int redis_port = 6379;
    int redis_db = 0;
    std::string redis_stream_key = "ssv:events";
    std::string redis_consumer_group = "ssv-agent";

    // Display
    bool display_enabled = false;
    std::string display_sink = "autovideosink";

    // Pipeline
    int analysis_fps = 5;
    int frame_width = 640;
    int frame_height = 480;

    // Inference
    std::string model_path;
    float confidence_threshold = 0.5f;
    std::string device = "cpu";
    std::string target_class = "person";

    // Tracking
    bool tracking_enabled = true;
    int tracking_frame_rate = 30;
    float tracking_threshold = 0.5f;
    int tracking_track_buffer = 30;
    float tracking_match_threshold = 0.8f;
    bool tracking_mock = false;

    // Agent
    int state_machine_timeout = 300;
    int max_retries = 3;
};

/// Load configuration from YAML file.
/// Search order: explicit path → SSV_CONFIG_PATH env → config/ssv.default.yaml → /etc/ssv/ssv.yaml
/// Throws std::runtime_error if no config file found.
SsvConfig ssv_config_load(const std::string& path = "");

} // namespace ssv
