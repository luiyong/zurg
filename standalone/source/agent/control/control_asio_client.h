#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>
#include <spdlog/logger.h>

#include "os.grpc.pb.h"
#include "runtime/task_scheduler.h"

namespace zurg::agent {

class ControlAsioClient {
 public:
  struct Options {
    std::function<bool()> should_run;
    std::function<std::chrono::milliseconds(std::size_t)> backoff_fn;
    std::function<void(std::chrono::milliseconds)> sleep_fn;
  };

  ControlAsioClient(ops::v1::Control::StubInterface* stub, std::string agent_id,
                    std::shared_ptr<runtime::TaskScheduler> scheduler, Options options,
                    std::shared_ptr<spdlog::logger> logger);
  ~ControlAsioClient();

  ControlAsioClient(const ControlAsioClient&) = delete;
  ControlAsioClient& operator=(const ControlAsioClient&) = delete;

  void Run();
  void Stop();

 private:
  bool ShouldContinue() const;
  void RunOnce();
  void HandleMessage(const ops::v1::ServerToAgent& msg);

  ops::v1::Control::StubInterface* stub_;
  std::string agent_id_;
  std::shared_ptr<runtime::TaskScheduler> scheduler_;
  Options options_;
  std::shared_ptr<spdlog::logger> logger_;
  std::atomic<bool> running_{false};
};

}  // namespace zurg::agent
