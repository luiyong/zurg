#pragma once

#ifndef GRPC_CALLBACK_API_NONEXPERIMENTAL
#define GRPC_CALLBACK_API_NONEXPERIMENTAL 1
#endif

#include <grpcpp/grpcpp.h>

#include "os.grpc.pb.h"

#include <chrono>
#include <functional>
#include <string>

namespace zurg { namespace agent {

void StartAgent(ops::v1::Control::StubInterface* stub, const std::string& agent_id);

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

}  // namespace internal

} }
