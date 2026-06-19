#include "agent/config/agent_config.h"

#include <exception>

#include <yaml-cpp/yaml.h>

namespace zurg::agent::config {

std::optional<AgentConfig> LoadAgentConfig(const std::string& path, std::string* error) {
  try {
    YAML::Node root = YAML::LoadFile(path);
    AgentConfig config;

    if (auto features = root["features"]) {
      FeatureToggles toggles;
      toggles.enabled = features["enabled"].as<bool>(toggles.enabled);
      toggles.enable_log_filter = features["log_filter"].as<bool>(toggles.enable_log_filter);
      toggles.enable_pcap = features["pcap"].as<bool>(toggles.enable_pcap);
      toggles.enable_exec = features["exec"].as<bool>(toggles.enable_exec);
      if (!toggles.enabled) {
        toggles.enable_log_filter = false;
        toggles.enable_pcap = false;
        toggles.enable_exec = false;
      }
      config.features = toggles;
    }

    if (auto logging = root["logging"]) {
      LoggingConfig logging_cfg;
      logging_cfg.file = logging["file"].as<std::string>(logging_cfg.file);
      logging_cfg.max_files = logging["max_files"].as<std::size_t>(logging_cfg.max_files);
      logging_cfg.max_size_bytes =
          logging["max_size_bytes"].as<std::size_t>(logging_cfg.max_size_bytes);
      config.logging = logging_cfg;
    }

    if (auto log_filter = root["log_filter"]) {
      zurg::log_ops::Options opts;
      opts.log_root = log_filter["log_root"].as<std::string>(opts.log_root);
      opts.temp_dir = log_filter["temp_dir"].as<std::string>(opts.temp_dir);
      opts.chunk_size = log_filter["chunk_size"].as<std::size_t>(opts.chunk_size);
      opts.cleanup_temp_file =
          log_filter["cleanup_temp_file"].as<bool>(opts.cleanup_temp_file);
      opts.base_path = log_filter["base_path"].as<std::string>(opts.base_path);
      opts.include_rotations =
          log_filter["include_rotations"].as<bool>(opts.include_rotations);
      opts.rotation_scan_depth =
          log_filter["rotation_scan_depth"].as<std::uint32_t>(opts.rotation_scan_depth);
      opts.output_basename =
          log_filter["output_basename"].as<std::string>(opts.output_basename);
      config.log_filter = opts;
    }

    if (auto auth = root["auth"]) {
      AuthConfig auth_cfg;
      auth_cfg.enabled = auth["enabled"].as<bool>(auth_cfg.enabled);
      auth_cfg.server = auth["server"].as<std::string>(auth_cfg.server);
      auth_cfg.persistence_file =
          auth["persistence_file"].as<std::string>(auth_cfg.persistence_file);
      auth_cfg.mode = auth["mode"].as<std::string>(auth_cfg.mode);
      config.auth = auth_cfg;
    }

    if (auto grpc = root["grpc"]) {
      config.grpc.enabled = grpc["enabled"].as<bool>(config.grpc.enabled);
      config.grpc.target = grpc["target"].as<std::string>(config.grpc.target);
    }

    if (auto http = root["http"]) {
      config.http.enabled = http["enabled"].as<bool>(config.http.enabled);
      config.http.listen_address =
          http["listen_address"].as<std::string>(config.http.listen_address);
      config.http.port = http["port"].as<int>(config.http.port);
    }

    if (auto persistence = root["persistence"]) {
      config.persistence.enabled =
          persistence["enabled"].as<bool>(config.persistence.enabled);
      config.persistence.path =
          persistence["path"].as<std::string>(config.persistence.path);
    }

    if (config.http.port <= 0 || config.http.port > 65535) {
      if (error) *error = "http.port must be in range 1..65535";
      return std::nullopt;
    }

    return config;
  } catch (const std::exception& ex) {
    if (error) {
      *error = ex.what();
    }
    return std::nullopt;
  }
}

}  // namespace zurg::agent::config
