#pragma once

#include <spdlog/common.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/base_sink.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace zurg::logging {

struct LoggerInfo {
  std::string name;
  spdlog::level::level_enum level = spdlog::level::info;
  bool active = false;
};

struct LoggerConfig {
  spdlog::level::level_enum default_level = spdlog::level::info;
  std::optional<std::string> pattern;
};

class LoggerManager {
 public:
  using sink_ptr = std::shared_ptr<spdlog::sinks::sink>;

  static void init(const LoggerConfig& config = {});

  static void add_sink(const sink_ptr& sink);

  static std::shared_ptr<spdlog::logger> logger(const std::string& name);

  static void set_level(const std::string& name, spdlog::level::level_enum level);

  static std::vector<LoggerInfo> list_loggers();

  static void shutdown();
};

}  // namespace zurg::logging

