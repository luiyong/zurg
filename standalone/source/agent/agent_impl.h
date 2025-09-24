#pragma once

#ifndef GRPC_CALLBACK_API_NONEXPERIMENTAL
#ifndef GRPC_CALLBACK_API_NONEXPERIMENTAL
#define GRPC_CALLBACK_API_NONEXPERIMENTAL 1
#endif
#endif

#include <grpcpp/grpcpp.h>

#include "os.grpc.pb.h"

#include <chrono>
#include <functional>
#include <string>

namespace zurg { namespace agent {

void StartAgent(ops::v1::Control::StubInterface* stub, const std::string& agent_id);

namespace internal {

void RequestStop();
void ResetForTests();
void SetBackoffHookForTests(std::function<std::chrono::milliseconds(std::size_t)> hook);
void SetSleepHookForTests(std::function<void(std::chrono::milliseconds)> hook);
void ClearTestHooks();

}  // namespace internal

} }
