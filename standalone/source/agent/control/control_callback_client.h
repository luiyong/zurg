#ifndef ZURG_AGENT_CONTROL_CONTROL_CALLBACK_CLIENT_H_
#define ZURG_AGENT_CONTROL_CONTROL_CALLBACK_CLIENT_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

#include <spdlog/logger.h>

#include "agent/agent_impl.h"
#include "control/control_stream_client.h"
#include "os.grpc.pb.h"
#include "tasks/task.h"
#include "zurg/agent/events.h"
#include "zurg/event/agent_events.h"
#include "zurg/log_ops.h"

namespace zurg::agent {

class ControlCallbackClient : public tasks::TaskContext {
 public:
  struct Options {
    zurg::log_ops::Options log_options;
    FeatureToggles features;
    std::function<bool()> should_run;
    std::function<std::chrono::milliseconds(std::size_t)> backoff_fn;
    std::function<void(std::chrono::milliseconds)> sleep_fn;
    std::function<void(const ops::v1::AgentToServer&)> on_send;
  };

  ControlCallbackClient(ops::v1::Control::StubInterface* stub, std::string agent_id,
                        Options options);
  ~ControlCallbackClient() override;

  void Run();
  void Stop();

  bool ShouldContinue() const override;
  void SendLogData(std::uint32_t op_id, ops::v1::LogChunk chunk) override;
  void SendEofLog(std::uint32_t op_id, const ops::v1::LogFilterEof& eof) override;
  void SendPcapData(std::uint32_t op_id, ops::v1::PcapPacket pkt) override;
  void SendEofPcap(std::uint32_t op_id, const ops::v1::PcapStats& stats) override;
  void SendError(std::uint32_t op_id, std::string code, std::string message) override;
  void SendExecData(std::uint32_t op_id, ops::v1::ExecChunk chunk) override;
  void SendEofExec(std::uint32_t op_id, const ops::v1::ExecExit& exit) override;

 private:
 bool ShouldStreamContinue() const;
  bool ShouldContinueInternal() const;
  void ShutdownWorker();
  void EnqueueWrite(ops::v1::AgentToServer msg);
  void SendAck(std::uint32_t op_id, bool accepted, std::string reason = std::string());
  void DoSendError(std::uint32_t op_id, std::string code, std::string message);
  void DoSendLogData(std::uint32_t op_id, ops::v1::LogChunk chunk);
  void DoSendPcapData(std::uint32_t op_id, ops::v1::PcapPacket pkt);
  void DoSendEofLog(std::uint32_t op_id, const ops::v1::LogFilterEof& eof);
  void DoSendEofPcap(std::uint32_t op_id, const ops::v1::PcapStats& stats);
  void DoSendExecData(std::uint32_t op_id, ops::v1::ExecChunk chunk);
  void DoSendEofExec(std::uint32_t op_id, const ops::v1::ExecExit& exit);
  void OnAuthStateChanged(const events::AuthStateChangedEvent& event);
  void ApplyFeatureUpdate(const FeatureToggles& toggles);
  bool IsTaskAllowed(const FeatureToggles& toggles, tasks::Task::Kind kind) const;
  FeatureToggles RestrictedFeatures() const;

  void HandleStreamMessage(const ops::v1::ServerToAgent& msg, bool ok);
  void HandleStreamClosed(const grpc::Status& status);
  void HandleStartOp(const ops::v1::StartOp& start);
  void HandleCancel(std::uint32_t op_id);
  void HandleShutdown(const ops::v1::Shutdown& shutdown);
  void CancelAllLocked(std::string_view reason);
  void RunTask(const std::shared_ptr<tasks::Task>& task);
  void WorkerLoop();

  ops::v1::Control::StubInterface* stub_;
  std::string agent_id_;
  Options options_;
  std::shared_ptr<spdlog::logger> logger_;
  std::unique_ptr<ControlStreamClient> stream_client_;

  std::atomic<bool> running_{false};
  std::thread worker_thread_;

  std::mutex mu_;
  std::condition_variable task_cv_;
  std::deque<std::shared_ptr<tasks::Task>> task_queue_;
  std::unordered_map<std::uint32_t, std::shared_ptr<tasks::Task>> tasks_;
  std::shared_ptr<tasks::Task> current_task_;
  bool drain_mode_ = false;
  bool stop_worker_ = false;
  FeatureToggles requested_features_;
  FeatureToggles active_features_;
  events::EventBus::SubscriptionToken auth_subscription_;
};

}  // namespace zurg::agent

#endif  // ZURG_AGENT_CONTROL_CONTROL_CALLBACK_CLIENT_H_
