#pragma once

#include <grpcpp/grpcpp.h>
#include <grpcpp/impl/codegen/server_callback_handlers.h>

#include "os.grpc.pb.h"
#include "zurg/log_ops.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

#include <memory>
#include <spdlog/sinks/sink.h>

namespace zurg { namespace agent {

struct FeatureToggles {
  bool enabled = true;
  bool enable_log_filter = true;
  bool enable_pcap = true;
  bool enable_exec = true;
};

void StartAgent(ops::v1::Control::StubInterface* stub, const std::string& agent_id);
void SetFeatureToggles(FeatureToggles toggles);
FeatureToggles GetFeatureToggles();

namespace internal {

bool IsRunning();
void RequestStop();
void ResetForTests();
void SetBackoffHookForTests(std::function<std::chrono::milliseconds(std::size_t)> hook);
void SetSleepHookForTests(std::function<void(std::chrono::milliseconds)> hook);
void ClearTestHooks();
std::chrono::milliseconds ComputeBackoff(std::size_t attempt);
void SleepWithStop(std::chrono::milliseconds delay);
void SetSendHookForTests(std::function<void(const ops::v1::AgentToServer&)> hook);
void SetAdditionalLoggerSink(std::shared_ptr<spdlog::sinks::sink> sink);
void SetLogOptions(zurg::log_ops::Options opts);
std::function<void(const ops::v1::AgentToServer&)> GetSendHook();
std::shared_ptr<spdlog::logger> GetLogger();

}  // namespace internal

} }
