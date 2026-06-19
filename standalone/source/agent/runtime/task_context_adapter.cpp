#include "runtime/task_context_adapter.h"

#include <utility>

namespace zurg::agent::runtime {
namespace {

template <typename T>
std::string SerializePayload(const T& message) {
  std::string out;
  message.SerializeToString(&out);
  return out;
}

}  // namespace

TaskContextAdapter::TaskContextAdapter(std::uint32_t op_id, tasks::Task::Kind task_kind,
                                       TaskInputSource source, std::vector<TaskSink*> sinks,
                                       std::function<bool()> should_continue)
    : op_id_(op_id),
      task_kind_(task_kind),
      source_(source),
      sinks_(std::move(sinks)),
      should_continue_(std::move(should_continue)) {}

bool TaskContextAdapter::ShouldContinue() const {
  return !should_continue_ || should_continue_();
}

void TaskContextAdapter::SendLogData(std::uint32_t op_id, ops::v1::LogChunk chunk) {
  TaskEvent event;
  event.op_id = op_id;
  event.task_kind = task_kind_;
  event.source = source_;
  event.event_kind = TaskEventKind::kData;
  event.payload_kind = TaskPayloadKind::kLogChunk;
  event.payload_bytes = SerializePayload(chunk);
  Publish(std::move(event));
}

void TaskContextAdapter::SendEofLog(std::uint32_t op_id, const ops::v1::LogFilterEof& eof) {
  TaskEvent event;
  event.op_id = op_id;
  event.task_kind = task_kind_;
  event.source = source_;
  event.event_kind = TaskEventKind::kEof;
  event.payload_kind = TaskPayloadKind::kLogEof;
  event.state = tasks::Task::State::kCompleted;
  event.payload_bytes = SerializePayload(eof);
  Publish(std::move(event));
}

void TaskContextAdapter::SendPcapData(std::uint32_t op_id, ops::v1::PcapPacket pkt) {
  TaskEvent event;
  event.op_id = op_id;
  event.task_kind = task_kind_;
  event.source = source_;
  event.event_kind = TaskEventKind::kData;
  event.payload_kind = TaskPayloadKind::kPcapPacket;
  event.payload_bytes = SerializePayload(pkt);
  Publish(std::move(event));
}

void TaskContextAdapter::SendEofPcap(std::uint32_t op_id, const ops::v1::PcapStats& stats) {
  TaskEvent event;
  event.op_id = op_id;
  event.task_kind = task_kind_;
  event.source = source_;
  event.event_kind = TaskEventKind::kEof;
  event.payload_kind = TaskPayloadKind::kPcapStats;
  event.state = tasks::Task::State::kCompleted;
  event.payload_bytes = SerializePayload(stats);
  Publish(std::move(event));
}

void TaskContextAdapter::SendError(std::uint32_t op_id, std::string code, std::string message) {
  TaskEvent event;
  event.op_id = op_id;
  event.task_kind = task_kind_;
  event.source = source_;
  event.event_kind = TaskEventKind::kError;
  event.state = code == "CANCELLED" ? tasks::Task::State::kCancelled : tasks::Task::State::kFailed;
  event.code = std::move(code);
  event.message = std::move(message);
  Publish(std::move(event));
}

void TaskContextAdapter::SendExecData(std::uint32_t op_id, ops::v1::ExecChunk chunk) {
  TaskEvent event;
  event.op_id = op_id;
  event.task_kind = task_kind_;
  event.source = source_;
  event.event_kind = TaskEventKind::kData;
  event.payload_kind = TaskPayloadKind::kExecChunk;
  event.payload_bytes = SerializePayload(chunk);
  Publish(std::move(event));
}

void TaskContextAdapter::SendEofExec(std::uint32_t op_id, const ops::v1::ExecExit& exit) {
  TaskEvent event;
  event.op_id = op_id;
  event.task_kind = task_kind_;
  event.source = source_;
  event.event_kind = TaskEventKind::kEof;
  event.payload_kind = TaskPayloadKind::kExecExit;
  event.state = tasks::Task::State::kCompleted;
  event.payload_bytes = SerializePayload(exit);
  Publish(std::move(event));
}

void TaskContextAdapter::PublishAccepted(bool accepted, std::string reason) {
  TaskEvent event;
  event.op_id = op_id_;
  event.task_kind = task_kind_;
  event.source = source_;
  event.event_kind = accepted ? TaskEventKind::kAccepted : TaskEventKind::kRejected;
  event.accepted = accepted;
  event.message = std::move(reason);
  event.state = accepted ? tasks::Task::State::kPending : tasks::Task::State::kFailed;
  Publish(std::move(event));
}

void TaskContextAdapter::PublishState(tasks::Task::State state) {
  TaskEvent event;
  event.op_id = op_id_;
  event.task_kind = task_kind_;
  event.source = source_;
  event.event_kind = TaskEventKind::kStateChanged;
  event.state = state;
  Publish(std::move(event));
}

void TaskContextAdapter::Publish(TaskEvent event) {
  for (auto* sink : sinks_) {
    if (sink) {
      sink->OnTaskEvent(event);
    }
  }
}

}  // namespace zurg::agent::runtime
