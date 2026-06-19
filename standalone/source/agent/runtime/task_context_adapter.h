#pragma once

#include <memory>
#include <functional>
#include <vector>

#include "runtime/task_sink.h"
#include "tasks/task.h"

namespace zurg::agent::runtime {

class TaskContextAdapter : public tasks::TaskContext {
 public:
  TaskContextAdapter(std::uint32_t op_id, tasks::Task::Kind task_kind,
                     TaskInputSource source, std::vector<TaskSink*> sinks,
                     std::function<bool()> should_continue);

  bool ShouldContinue() const override;
  void SendLogData(std::uint32_t op_id, ops::v1::LogChunk chunk) override;
  void SendEofLog(std::uint32_t op_id, const ops::v1::LogFilterEof& eof) override;
  void SendPcapData(std::uint32_t op_id, ops::v1::PcapPacket pkt) override;
  void SendEofPcap(std::uint32_t op_id, const ops::v1::PcapStats& stats) override;
  void SendError(std::uint32_t op_id, std::string code, std::string message) override;
  void SendExecData(std::uint32_t op_id, ops::v1::ExecChunk chunk) override;
  void SendEofExec(std::uint32_t op_id, const ops::v1::ExecExit& exit) override;

  void PublishAccepted(bool accepted, std::string reason = {});
  void PublishState(tasks::Task::State state);

 private:
  void Publish(TaskEvent event);

  std::uint32_t op_id_;
  tasks::Task::Kind task_kind_;
  TaskInputSource source_;
  std::vector<TaskSink*> sinks_;
  std::function<bool()> should_continue_;
};

}  // namespace zurg::agent::runtime
