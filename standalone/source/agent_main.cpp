#include "agent/agent_impl.h"

#include "os.grpc.pb.h"

#include <cxxopts.hpp>
#include <fmt/core.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <chrono>
#include <grpcpp/grpcpp.h>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <string>

#include <spdlog/sinks/rotating_file_sink.h>

namespace {

std::string GenerateFallbackAgentId() {
  using namespace std::chrono;
  auto now = system_clock::now().time_since_epoch();
  return fmt::format("agent-{}", duration_cast<milliseconds>(now).count());
}

grpc::ChannelArguments MakeChannelArgs() {
  grpc::ChannelArguments args;
  args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, 5000);
  args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 10000);
  args.SetInt(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
  args.SetInt(GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA, 0);
  args.SetInt(GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS, 5000);
  return args;
}

struct LoggingConfig {
  std::string file;
  std::size_t max_files = 5;
  std::size_t max_size_bytes = 10 * 1024 * 1024;
};

struct AgentConfig {
  std::optional<zurg::agent::FeatureToggles> features;
  std::optional<LoggingConfig> logging;
  std::optional<zurg::log_ops::Options> log_filter;
};

std::optional<AgentConfig> LoadAgentConfig(const std::string& path, std::string* error) {
  try {
    YAML::Node root = YAML::LoadFile(path);
    AgentConfig config;

    if (auto features = root["features"]) {
      zurg::agent::FeatureToggles toggles;
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
      logging_cfg.max_size_bytes = logging["max_size_bytes"].as<std::size_t>(logging_cfg.max_size_bytes);
      config.logging = logging_cfg;
    }

    if (auto log_filter = root["log_filter"]) {
      zurg::log_ops::Options opts;
      opts.log_root = log_filter["log_root"].as<std::string>(opts.log_root);
      opts.temp_dir = log_filter["temp_dir"].as<std::string>(opts.temp_dir);
      opts.chunk_size = log_filter["chunk_size"].as<std::size_t>(opts.chunk_size);
      opts.cleanup_temp_file = log_filter["cleanup_temp_file"].as<bool>(opts.cleanup_temp_file);
      opts.base_path = log_filter["base_path"].as<std::string>(opts.base_path);
      opts.include_rotations = log_filter["include_rotations"].as<bool>(opts.include_rotations);
      opts.rotation_scan_depth = log_filter["rotation_scan_depth"].as<std::uint32_t>(opts.rotation_scan_depth);
      opts.output_basename = log_filter["output_basename"].as<std::string>(opts.output_basename);
      config.log_filter = opts;
    }

    return config;
  } catch (const std::exception& ex) {
    if (error) {
      *error = ex.what();
    }
    return std::nullopt;
  }
}

}  // namespace

int main(int argc, char** argv) {
  cxxopts::Options options("GnoiAgent", "Zurg gNOI agent");
  std::string target;
  std::string agent_id;
  std::string config_path;

  options.add_options()
      ("t,target", "gRPC server address", cxxopts::value(target)->default_value("127.0.0.1:50051"))
      ("a,agent_id", "agent identifier", cxxopts::value(agent_id))
      ("c,config", "YAML config file", cxxopts::value(config_path))
      ("h,help", "show help");

  const auto result = options.parse(argc, argv);
  if (result.count("help")) {
    fmt::print("{}\n", options.help());
    return 0;
  }

  if (!result.count("agent_id") || agent_id.empty()) {
    agent_id = GenerateFallbackAgentId();
  }

  std::optional<LoggingConfig> logging_cfg;
  if (!config_path.empty()) {
    if (!std::filesystem::exists(config_path)) {
      fmt::print(stderr, "[agent] config file '{}' not found\n", config_path);
      return 1;
    }
    std::string error;
    auto config = LoadAgentConfig(config_path, &error);
    if (!config) {
      fmt::print(stderr, "[agent] failed to load config '{}': {}\n", config_path, error);
      return 1;
    }
    if (config->features) {
      zurg::agent::SetFeatureToggles(*config->features);
    }
    if (config->log_filter) {
      if (logging_cfg && config->log_filter->include_rotations && config->log_filter->rotation_scan_depth == 0) {
        config->log_filter->rotation_scan_depth = static_cast<std::uint32_t>(logging_cfg->max_files);
      }
      if (config->log_filter->base_path.empty() && logging_cfg && !logging_cfg->file.empty()) {
        config->log_filter->base_path = logging_cfg->file;
      }
      zurg::agent::internal::SetLogOptions(*config->log_filter);
    } else if (logging_cfg && !logging_cfg->file.empty()) {
      zurg::log_ops::Options opts;
      opts.base_path = logging_cfg->file;
      opts.include_rotations = true;
      opts.rotation_scan_depth = static_cast<std::uint32_t>(logging_cfg->max_files);
      zurg::agent::internal::SetLogOptions(std::move(opts));
    }
    if (config->logging) {
      logging_cfg = *config->logging;
      logging_cfg->max_files = std::max<std::size_t>(1, logging_cfg->max_files);
      logging_cfg->max_size_bytes = std::max<std::size_t>(1024, logging_cfg->max_size_bytes);
    }
  }

  auto active_toggles = zurg::agent::GetFeatureToggles();

  if (logging_cfg && !logging_cfg->file.empty()) {
    try {
      std::filesystem::path log_path(logging_cfg->file);
      if (!log_path.parent_path().empty()) {
        std::filesystem::create_directories(log_path.parent_path());
      }
      auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
          logging_cfg->file, static_cast<std::size_t>(logging_cfg->max_size_bytes),
          static_cast<std::size_t>(logging_cfg->max_files));
      zurg::agent::internal::SetAdditionalLoggerSink(file_sink);
    } catch (const std::exception& ex) {
      fmt::print(stderr, "[agent] failed to configure file logging: {}\n", ex.what());
    }
  }

  auto channel = grpc::CreateCustomChannel(target, grpc::InsecureChannelCredentials(), MakeChannelArgs());
  auto stub = ops::v1::Control::NewStub(channel);

  fmt::print("[agent] starting with id '{}' connecting to {}\n", agent_id, target);
  fmt::print("[agent] feature toggles: enabled={} log_filter={} pcap={} exec={}\n",
             active_toggles.enabled ? "on" : "off",
             active_toggles.enable_log_filter ? "on" : "off",
             active_toggles.enable_pcap ? "on" : "off",
             active_toggles.enable_exec ? "on" : "off");
  if (logging_cfg && !logging_cfg->file.empty()) {
    fmt::print("[agent] logging to '{}' (max_files={}, max_size={} bytes)\n",
               logging_cfg->file, logging_cfg->max_files, logging_cfg->max_size_bytes);
  }
  zurg::agent::StartAgent(stub.get(), agent_id);
  fmt::print("[agent] terminated\n");
  return 0;
}
