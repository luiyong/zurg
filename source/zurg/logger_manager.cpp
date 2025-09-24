#include "zurg/logger_manager.h"

#include <spdlog/sinks/sink.h>
#include <spdlog/spdlog.h>

#include <mutex>
#include <unordered_map>

namespace zurg::logging {

namespace {

struct ManagerState {
  std::mutex mu;
  std::vector<LoggerManager::sink_ptr> sinks;
  std::unordered_map<std::string, std::weak_ptr<spdlog::logger>> loggers;
  std::unordered_map<std::string, spdlog::level::level_enum> pending_levels;
  LoggerConfig config;
  bool initialized = false;
  bool shutting_down = false;
};

ManagerState& state() {
  static ManagerState* inst = new ManagerState();
  return *inst;
}

void cleanup_expired_locked(ManagerState& st) {
  for (auto it = st.loggers.begin(); it != st.loggers.end();) {
    if (it->second.expired()) {
      it = st.loggers.erase(it);
    } else {
      ++it;
    }
  }
}

void remove_logger_internal(const std::string& name, const spdlog::logger* ptr) {
  auto& st = state();
  std::lock_guard<std::mutex> lock(st.mu);
  auto it = st.loggers.find(name);
  if (it == st.loggers.end()) return;
  auto locked = it->second.lock();
  if (locked && locked.get() != ptr) {
    return;
  }
  st.loggers.erase(it);
}

std::shared_ptr<spdlog::logger> make_logger_locked(ManagerState& st, const std::string& name) {
  auto it = st.loggers.find(name);
  if (it != st.loggers.end()) {
    if (auto existing = it->second.lock()) {
      return existing;
    }
  }

  auto sinks_copy = st.sinks;

  auto raw = new spdlog::logger(name, sinks_copy.begin(), sinks_copy.end());
  auto deleter = [name](spdlog::logger* logger_ptr) {
    remove_logger_internal(name, logger_ptr);
    delete logger_ptr;
  };
  std::shared_ptr<spdlog::logger> created(raw, deleter);

  auto pending = st.pending_levels.find(name);
  auto level = pending == st.pending_levels.end() ? st.config.default_level : pending->second;
  created->set_level(level);
  if (pending != st.pending_levels.end()) {
    st.pending_levels.erase(pending);
  }
  if (st.config.pattern.has_value()) {
    created->set_pattern(*st.config.pattern);
  }

  st.loggers[name] = created;
  return created;
}

}  // namespace

void LoggerManager::init(const LoggerConfig& config) {
  auto& st = state();
  std::lock_guard<std::mutex> lock(st.mu);
  st.config = config;
  st.initialized = true;
  st.shutting_down = false;
  cleanup_expired_locked(st);
  for (auto& [name, weak_logger] : st.loggers) {
    if (auto logger = weak_logger.lock()) {
      logger->set_level(st.config.default_level);
      if (st.config.pattern.has_value()) {
        logger->set_pattern(*st.config.pattern);
      }
    }
  }
}

void LoggerManager::add_sink(const sink_ptr& sink) {
  if (!sink) return;
  auto& st = state();
  std::lock_guard<std::mutex> lock(st.mu);
  if (st.shutting_down) return;
  st.sinks.push_back(sink);
  cleanup_expired_locked(st);
  for (auto& [_, weak_logger] : st.loggers) {
    if (auto logger = weak_logger.lock()) {
      logger->sinks().push_back(sink);
    }
  }
}

std::shared_ptr<spdlog::logger> LoggerManager::logger(const std::string& name) {
  auto& st = state();
  std::lock_guard<std::mutex> lock(st.mu);
  if (st.shutting_down) {
    return nullptr;
  }
  cleanup_expired_locked(st);
  return make_logger_locked(st, name);
}

void LoggerManager::set_level(const std::string& name, spdlog::level::level_enum level) {
  auto& st = state();
  std::lock_guard<std::mutex> lock(st.mu);
  if (st.shutting_down) return;
  cleanup_expired_locked(st);
  auto it = st.loggers.find(name);
  if (it != st.loggers.end()) {
    if (auto logger = it->second.lock()) {
      logger->set_level(level);
      return;
    }
  }
  st.pending_levels[name] = level;
}

std::vector<LoggerInfo> LoggerManager::list_loggers() {
  auto& st = state();
  std::vector<LoggerInfo> out;
  std::lock_guard<std::mutex> lock(st.mu);
  cleanup_expired_locked(st);
  out.reserve(st.loggers.size());
  for (auto it = st.loggers.begin(); it != st.loggers.end(); ++it) {
    LoggerInfo info;
    info.name = it->first;
    if (auto logger = it->second.lock()) {
      info.active = true;
      info.level = logger->level();
    } else {
      info.active = false;
    }
    out.push_back(info);
  }
  return out;
}

void LoggerManager::shutdown() {
  auto& st = state();
  std::lock_guard<std::mutex> lock(st.mu);
  st.loggers.clear();
  st.pending_levels.clear();
  st.sinks.clear();
  st.initialized = false;
  st.shutting_down = true;
  st.config = LoggerConfig{};
  spdlog::shutdown();
}

}  // namespace zurg::logging
