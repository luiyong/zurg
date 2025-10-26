#include "agent_impl.h"

#include "zurg/log_ops.h"
#include "zurg/logger_manager.h"
#include "zurg/pcap_ops.h"
#include "control/control_callback_client.h"

#include <spdlog/sinks/stdout_color_sinks.h>

#include <cstdio>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <system_error>
#include <mutex>
#include <optional>
#include <string_view>
#include <string>
#include <thread>
#include <unordered_map>

namespace zurg::agent {

namespace internal {

namespace {
std::atomic<bool> g_running{true};
std::mutex g_hook_mu;
std::function<std::chrono::milliseconds(std::size_t)> g_backoff_hook;
std::function<void(std::chrono::milliseconds)> g_sleep_hook;
std::function<void(const ops::v1::AgentToServer&)> g_send_hook;
std::once_flag g_logger_once;
std::shared_ptr<spdlog::logger> g_logger;
std::shared_ptr<spdlog::sinks::sink> g_logger_sink;
std::optional<zurg::log_ops::Options> g_log_options_override;
FeatureToggles g_feature_toggles;
}

bool IsRunning() { return g_running.load(); }

void RequestStop() { g_running.store(false); }

void ClearTestHooks() {
  std::lock_guard<std::mutex> lock(g_hook_mu);
  g_backoff_hook = nullptr;
  g_sleep_hook = nullptr;
  g_send_hook = nullptr;
}

void ResetForTests() {
  g_running.store(true);
  ClearTestHooks();
  {
    std::lock_guard<std::mutex> lock(g_hook_mu);
    g_log_options_override.reset();
    g_feature_toggles = FeatureToggles{};
  }
}

void SetBackoffHookForTests(std::function<std::chrono::milliseconds(std::size_t)> hook) {
  std::lock_guard<std::mutex> lock(g_hook_mu);
  g_backoff_hook = std::move(hook);
}

void SetSleepHookForTests(std::function<void(std::chrono::milliseconds)> hook) {
  std::lock_guard<std::mutex> lock(g_hook_mu);
  g_sleep_hook = std::move(hook);
}

void SetSendHookForTests(std::function<void(const ops::v1::AgentToServer&)> hook) {
  std::lock_guard<std::mutex> lock(g_hook_mu);
  g_send_hook = std::move(hook);
}

std::function<void(const ops::v1::AgentToServer&)> GetSendHook() {
  std::lock_guard<std::mutex> lock(g_hook_mu);
  return g_send_hook;
}

void SetLogOptions(zurg::log_ops::Options opts) {
  std::lock_guard<std::mutex> lock(g_hook_mu);
  g_log_options_override = std::move(opts);
}

std::optional<zurg::log_ops::Options> GetLogOptionsOverride() {
  std::lock_guard<std::mutex> lock(g_hook_mu);
  return g_log_options_override;
}

static FeatureToggles GetFeatureTogglesInternal() {
  std::lock_guard<std::mutex> lock(g_hook_mu);
  return g_feature_toggles;
}

static void SetFeatureTogglesInternal(FeatureToggles toggles) {
  if (!toggles.enabled) {
    toggles.enable_log_filter = false;
    toggles.enable_pcap = false;
    toggles.enable_exec = false;
  }
  std::lock_guard<std::mutex> lock(g_hook_mu);
  g_feature_toggles = std::move(toggles);
}

std::shared_ptr<spdlog::logger> GetLogger() {
  std::call_once(g_logger_once, [] {
    logging::LoggerManager::init({});
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    logging::LoggerManager::add_sink(console_sink);
    if (g_logger_sink) {
      logging::LoggerManager::add_sink(g_logger_sink);
    }
    g_logger = logging::LoggerManager::logger("agent.callback");
  });
  return g_logger;
}

void SetAdditionalLoggerSink(std::shared_ptr<spdlog::sinks::sink> sink) {
  std::lock_guard<std::mutex> lock(g_hook_mu);
  g_logger_sink = std::move(sink);
}

std::chrono::milliseconds ComputeBackoff(std::size_t attempt) {
  std::function<std::chrono::milliseconds(std::size_t)> hook;
  {
    std::lock_guard<std::mutex> lock(g_hook_mu);
    hook = g_backoff_hook;
  }
  if (hook) {
    return hook(attempt);
  }
  using namespace std::chrono;
  constexpr auto base = 500ms;
  constexpr auto max_delay = 30s;
  auto multiplier = static_cast<std::size_t>(1) << std::min<std::size_t>(attempt, 6);
  auto delay = base * multiplier;
  if (delay > max_delay) delay = max_delay;
  return delay;
}

void SleepWithStop(std::chrono::milliseconds delay) {
  std::function<void(std::chrono::milliseconds)> hook;
  {
    std::lock_guard<std::mutex> lock(g_hook_mu);
    hook = g_sleep_hook;
  }
  if (hook) {
    hook(delay);
    return;
  }
  constexpr std::chrono::milliseconds step{200};
  auto remaining = delay;
  while (IsRunning() && remaining.count() > 0) {
    auto chunk = remaining > step ? step : remaining;
    std::this_thread::sleep_for(chunk);
    remaining -= chunk;
  }
}

}  // namespace internal

FeatureToggles GetFeatureToggles() {
  return internal::GetFeatureTogglesInternal();
}

void SetFeatureToggles(FeatureToggles toggles) {
  internal::SetFeatureTogglesInternal(std::move(toggles));
}

void HandleSignal(int signo) {
  if (signo == SIGINT || signo == SIGTERM) {
    internal::RequestStop();
  }
}

void StartAgent(ops::v1::Control::StubInterface* stub, const std::string& agent_id) {
  if (!stub) {
    auto logger = internal::GetLogger();
    if (logger) {
      logger->error("missing stub, aborting agent startup");
    } else {
      std::fprintf(stderr, "[agent] missing stub, aborting\n");
    }
    return;
  }

  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);

  ControlCallbackClient::Options options;
  options.should_run = [] { return internal::IsRunning(); };
  options.backoff_fn = [](std::size_t attempt) { return internal::ComputeBackoff(attempt); };
  options.sleep_fn = [](std::chrono::milliseconds delay) { internal::SleepWithStop(delay); };
  if (auto override_opts = internal::GetLogOptionsOverride()) {
    options.log_options = *override_opts;
  }
  options.features = GetFeatureToggles();

  ControlCallbackClient client(stub, agent_id, options);
  client.Run();
}

}  // namespace zurg::agent
