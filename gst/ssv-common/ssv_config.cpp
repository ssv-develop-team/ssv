#include "ssv_config.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include <yaml-cpp/yaml.h>

namespace ssv {

namespace {

std::string find_config_file(const std::string& explicit_path) {
    if (!explicit_path.empty()) {
        if (std::filesystem::exists(explicit_path))
            return explicit_path;
        throw std::runtime_error("Config file not found: " + explicit_path);
    }

    // SSV_CONFIG_PATH environment variable
    const char* env_path = std::getenv("SSV_CONFIG_PATH");
    if (env_path && std::filesystem::exists(env_path))
        return env_path;

    // Relative to working directory
    if (std::filesystem::exists("config/ssv.default.yaml"))
        return "config/ssv.default.yaml";

    // System-wide fallback
    if (std::filesystem::exists("/etc/ssv/ssv.yaml"))
        return "/etc/ssv/ssv.yaml";

    throw std::runtime_error(
        "No config file found. Searched: SSV_CONFIG_PATH, "
        "config/ssv.default.yaml, /etc/ssv/ssv.yaml");
}

template <typename T>
T get_or(const YAML::Node& node, const std::string& key, const T& default_val) {
    if (node && node[key])
        return node[key].as<T>();
    return default_val;
}

} // anonymous namespace

SsvConfig ssv_config_load(const std::string& path) {
    std::string resolved = find_config_file(path);
    YAML::Node root = YAML::LoadFile(resolved);

    SsvConfig cfg;

    // Logging
    if (auto logging = root["logging"]) {
        cfg.cpp_debug_level = get_or<std::string>(logging, "cpp_debug_level", cfg.cpp_debug_level);
        cfg.python_log_level = get_or<std::string>(logging, "python_log_level", cfg.python_log_level);
    }

    // Redis
    if (auto redis = root["redis"]) {
        cfg.redis_host = get_or<std::string>(redis, "host", cfg.redis_host);
        cfg.redis_port = get_or<int>(redis, "port", cfg.redis_port);
        cfg.redis_db = get_or<int>(redis, "db", cfg.redis_db);
        cfg.redis_stream_key = get_or<std::string>(redis, "stream_key", cfg.redis_stream_key);
        cfg.redis_consumer_group = get_or<std::string>(redis, "consumer_group", cfg.redis_consumer_group);
    }

    // Display
    if (auto display = root["display"]) {
        cfg.display_enabled = get_or<bool>(display, "enabled", cfg.display_enabled);
        cfg.display_sink = get_or<std::string>(display, "sink", cfg.display_sink);
    }

    // Pipeline
    if (auto pipeline = root["pipeline"]) {
        cfg.analysis_fps = get_or<int>(pipeline, "analysis_fps", cfg.analysis_fps);
        cfg.frame_width = get_or<int>(pipeline, "frame_width", cfg.frame_width);
        cfg.frame_height = get_or<int>(pipeline, "frame_height", cfg.frame_height);
    }

    // Inference
    if (auto inference = root["inference"]) {
        cfg.model_path = get_or<std::string>(inference, "model_path", cfg.model_path);
        cfg.confidence_threshold = get_or<float>(inference, "confidence_threshold", cfg.confidence_threshold);
        cfg.device = get_or<std::string>(inference, "device", cfg.device);
        cfg.target_class = get_or<std::string>(inference, "target_class", cfg.target_class);
    }

    // Tracking
    if (auto tracking = root["tracking"]) {
        cfg.tracking_enabled = get_or<bool>(tracking, "enabled", cfg.tracking_enabled);
        cfg.tracking_frame_rate = get_or<int>(tracking, "frame_rate", cfg.tracking_frame_rate);
        cfg.tracking_threshold = get_or<float>(tracking, "track_threshold", cfg.tracking_threshold);
        cfg.tracking_track_buffer = get_or<int>(tracking, "track_buffer", cfg.tracking_track_buffer);
        cfg.tracking_match_threshold = get_or<float>(tracking, "match_threshold", cfg.tracking_match_threshold);
        cfg.tracking_mock = get_or<bool>(tracking, "mock_track", cfg.tracking_mock);
    }

    // Agent
    if (auto agent = root["agent"]) {
        cfg.state_machine_timeout = get_or<int>(agent, "state_machine_timeout", cfg.state_machine_timeout);
        cfg.max_retries = get_or<int>(agent, "max_retries", cfg.max_retries);
    }

    return cfg;
}

} // namespace ssv
