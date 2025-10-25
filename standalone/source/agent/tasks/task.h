#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "os.pb.h"

namespace spdlog {
class logger;
}

namespace zurg::agent::tasks {

class TaskContext {
 public:
  virtual ~TaskContext() = default;
  virtual bool ShouldContinue() const = 0;
  virtual void SendLogData(std::uint32_t op_id, ops::v1::LogChunk chunk) = 0;
  virtual void SendEofLog(std::uint32_t op_id, const ops::v1::LogFilterEof& eof) = 0;
  virtual void SendPcapData(std::uint32_t op_id, ops::v1::PcapPacket pkt) = 0;
  virtual void SendEofPcap(std::uint32_t op_id, const ops::v1::PcapStats& stats) = 0;
  virtual void SendError(std::uint32_t op_id, std::string code, std::string message) = 0;
  virtual void SendExecData(std::uint32_t op_id, ops::v1::ExecChunk chunk) = 0;
  virtual void SendEofExec(std::uint32_t op_id, const ops::v1::ExecExit& exit) = 0;
};

class Task : public std::enable_shared_from_this<Task> {
 public:
  enum class Kind { kLogFilter, kPcap, kExec };
  enum class State { kPending, kRunning, kPaused, kCompleted, kCancelled, kFailed };

  Task(std::uint32_t op_id, Kind kind, std::shared_ptr<spdlog::logger> logger);
  virtual ~Task();

  std::uint32_t op_id() const { return op_id_; }
  Kind kind() const { return kind_; }
  State state() const { return state_.load(); }

  void RequestCancel() { cancel_requested_.store(true); }
  void RequestPause() { pause_requested_.store(true); }
  bool CancelRequested() const { return cancel_requested_.load(); }
  bool PauseRequested() const { return pause_requested_.load(); }
  void SetState(State state) { state_.store(state); }

  virtual bool Validate(std::string* reason) const { (void)reason; return true; }

  virtual void Run(TaskContext& ctx) = 0;

 protected:
  std::shared_ptr<spdlog::logger> logger_;

 private:
  std::uint32_t op_id_;
  Kind kind_;
  std::atomic<State> state_{State::kPending};
  std::atomic<bool> cancel_requested_{false};
  std::atomic<bool> pause_requested_{false};
};

using TaskPtr = std::shared_ptr<Task>;

}  // namespace zurg::agent::tasks
