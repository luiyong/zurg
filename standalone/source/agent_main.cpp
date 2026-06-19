#include "agent/agent_impl.h"

#include "agent/config/agent_config.h"
#include "os.grpc.pb.h"
#include "zurg/auth/auth_manager.h"

#include <cxxopts.hpp>
#include <fmt/core.h>
#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <grpcpp/grpcpp.h>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <string>

#include <spdlog/sinks/rotating_file_sink.h>

namespace {

std::string ToLowerCopy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

const char* AuthModeName(zurg::auth::AuthMode mode) {
  switch (mode) {
    case zurg::auth::AuthMode::kOnline:
      return "online";
    case zurg::auth::AuthMode::kOffline:
      return "offline";
    case zurg::auth::AuthMode::kUnknown:
    default:
      return "unknown";
  }
}

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

  std::optional<zurg::agent::config::AgentConfig> agent_config;
  std::optional<zurg::agent::config::LoggingConfig> logging_cfg;
  if (!config_path.empty()) {
    if (!std::filesystem::exists(config_path)) {
      fmt::print(stderr, "[agent] config file '{}' not found\n", config_path);
      return 1;
    }
    std::string error;
    agent_config = zurg::agent::config::LoadAgentConfig(config_path, &error);
    if (!agent_config) {
      fmt::print(stderr, "[agent] failed to load config '{}': {}\n", config_path, error);
      return 1;
    }
    if (agent_config->features) {
      zurg::agent::SetFeatureToggles(*agent_config->features);
    }
    if (!result.count("target")) {
      target = agent_config->grpc.target;
    }
    if (agent_config->logging) {
      logging_cfg = *agent_config->logging;
      logging_cfg->max_files = std::max<std::size_t>(1, logging_cfg->max_files);
      logging_cfg->max_size_bytes = std::max<std::size_t>(1024, logging_cfg->max_size_bytes);
    }
    if (agent_config->log_filter) {
      auto opts = *agent_config->log_filter;
      if (logging_cfg && opts.include_rotations && opts.rotation_scan_depth == 0) {
        opts.rotation_scan_depth = static_cast<std::uint32_t>(logging_cfg->max_files);
      }
      if (opts.base_path.empty() && logging_cfg && !logging_cfg->file.empty()) {
        opts.base_path = logging_cfg->file;
      }
      zurg::agent::internal::SetLogOptions(opts);
    } else if (logging_cfg && !logging_cfg->file.empty()) {
      zurg::log_ops::Options opts;
      opts.base_path = logging_cfg->file;
      opts.include_rotations = true;
      opts.rotation_scan_depth = static_cast<std::uint32_t>(logging_cfg->max_files);
      zurg::agent::internal::SetLogOptions(std::move(opts));
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

  auto channel =
      grpc::CreateCustomChannel(target, grpc::InsecureChannelCredentials(), MakeChannelArgs());
  auto stub = ops::v1::Control::NewStub(channel);

  std::unique_ptr<zurg::auth::AuthManager> auth_manager;
  std::string auth_status_message;
  if (agent_config && agent_config->auth) {
    const auto& auth_cfg = *agent_config->auth;
    if (auth_cfg.enabled) {
      std::string auth_server = auth_cfg.server.empty() ? target : auth_cfg.server;
      auth_manager = std::make_unique<zurg::auth::AuthManager>(auth_server, agent_id);
      if (!auth_cfg.persistence_file.empty()) {
        auth_manager->SetPersistenceFile(auth_cfg.persistence_file);
      }
      zurg::auth::AuthMode configured_mode = zurg::auth::AuthMode::kOnline;
      bool mode_overridden = false;
      if (!auth_cfg.mode.empty()) {
        const auto mode_lower = ToLowerCopy(auth_cfg.mode);
        if (mode_lower == "offline") {
          configured_mode = zurg::auth::AuthMode::kOffline;
          mode_overridden = true;
        } else if (mode_lower == "unknown") {
          configured_mode = zurg::auth::AuthMode::kUnknown;
          mode_overridden = true;
        }
      }
      if (mode_overridden) {
        auth_manager->SetMode(configured_mode);
      }
      auth_manager->Start();
      const auto effective_mode = mode_overridden ? configured_mode : auth_manager->GetMode();
      const bool authorized = auth_manager->CheckAuthorization();
      auth_status_message = fmt::format(
          "[agent] auth enabled: server={} mode={} persistence={} authorized={}\n",
          auth_server, AuthModeName(effective_mode),
          auth_cfg.persistence_file.empty() ? "(memory)" : auth_cfg.persistence_file,
          authorized ? "true" : "false");
    } else {
      auth_status_message = "[agent] auth disabled by config\n";
    }
  } else {
    auth_status_message = "[agent] auth not configured (disabled)\n";
  }

  fmt::print("[agent] starting with id '{}' connecting to {}\n", agent_id, target);
  fmt::print("[agent] feature toggles: enabled={} log_filter={} pcap={} exec={}\n",
             active_toggles.enabled ? "on" : "off",
             active_toggles.enable_log_filter ? "on" : "off",
             active_toggles.enable_pcap ? "on" : "off",
             active_toggles.enable_exec ? "on" : "off");
  fmt::print("{}", auth_status_message);
  if (agent_config) {
    fmt::print("[agent] grpc input: enabled={} target={}\n",
               agent_config->grpc.enabled ? "on" : "off", agent_config->grpc.target);
    fmt::print("[agent] http input: enabled={} listen={}:{}\n",
               agent_config->http.enabled ? "on" : "off", agent_config->http.listen_address,
               agent_config->http.port);
    fmt::print("[agent] persistence: enabled={} path={}\n",
               agent_config->persistence.enabled ? "on" : "off",
               agent_config->persistence.path);
  }
  if (logging_cfg && !logging_cfg->file.empty()) {
    fmt::print("[agent] logging to '{}' (max_files={}, max_size={} bytes)\n",
               logging_cfg->file, logging_cfg->max_files, logging_cfg->max_size_bytes);
  }
  zurg::agent::StartAgent(stub.get(), agent_id);
  if (auth_manager) {
    auth_manager->Stop();
  }
  fmt::print("[agent] terminated\n");
  return 0;
}
