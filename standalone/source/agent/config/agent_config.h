#pragma once

#include <cstddef>
#include <optional>
#include <string>

#include "runtime/feature_toggles.h"
#include "zurg/log_ops.h"

namespace zurg::agent::config {

struct LoggingConfig {
  std::string file;
  std::size_t max_files = 5;
  std::size_t max_size_bytes = 10 * 1024 * 1024;
};

struct GrpcConfig {
  std::string target = "127.0.0.1:50051";
  bool enabled = true;
};

struct HttpConfig {
  bool enabled = true;
  std::string listen_address = "127.0.0.1";
  int port = 8080;
};

struct PersistenceConfig {
  bool enabled = true;
  std::string path = "/tmp/zurg/tasks";
};

struct AuthConfig {
  bool enabled = false;
  std::string server;
  std::string persistence_file;
  std::string mode;
};

struct AgentConfig {
  std::optional<FeatureToggles> features;
  std::optional<LoggingConfig> logging;
  std::optional<zurg::log_ops::Options> log_filter;
  std::optional<AuthConfig> auth;
  GrpcConfig grpc;
  HttpConfig http;
  PersistenceConfig persistence;
};

std::optional<AgentConfig> LoadAgentConfig(const std::string& path, std::string* error);

}  // namespace zurg::agent::config
