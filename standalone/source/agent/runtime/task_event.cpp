#include "runtime/task_event.h"

#include <stdexcept>

namespace zurg::agent::runtime {

const char* ToString(TaskInputSource source) {
  switch (source) {
    case TaskInputSource::kGrpc:
      return "grpc";
    case TaskInputSource::kHttp:
      return "http";
    case TaskInputSource::kRecovered:
      return "recovered";
  }
  return "unknown";
}

const char* ToString(TaskEventKind kind) {
  switch (kind) {
    case TaskEventKind::kAccepted:
      return "accepted";
    case TaskEventKind::kRejected:
      return "rejected";
    case TaskEventKind::kData:
      return "data";
    case TaskEventKind::kEof:
      return "eof";
    case TaskEventKind::kError:
      return "error";
    case TaskEventKind::kStateChanged:
      return "state_changed";
  }
  return "unknown";
}

const char* ToString(TaskPayloadKind kind) {
  switch (kind) {
    case TaskPayloadKind::kNone:
      return "none";
    case TaskPayloadKind::kLogChunk:
      return "log_chunk";
    case TaskPayloadKind::kPcapPacket:
      return "pcap_packet";
    case TaskPayloadKind::kExecChunk:
      return "exec_chunk";
    case TaskPayloadKind::kLogEof:
      return "log_eof";
    case TaskPayloadKind::kPcapStats:
      return "pcap_stats";
    case TaskPayloadKind::kExecExit:
      return "exec_exit";
  }
  return "unknown";
}

const char* ToString(tasks::Task::Kind kind) {
  switch (kind) {
    case tasks::Task::Kind::kLogFilter:
      return "log_filter";
    case tasks::Task::Kind::kPcap:
      return "pcap";
    case tasks::Task::Kind::kExec:
      return "exec";
  }
  return "unknown";
}

const char* ToString(tasks::Task::State state) {
  switch (state) {
    case tasks::Task::State::kPending:
      return "pending";
    case tasks::Task::State::kRunning:
      return "running";
    case tasks::Task::State::kPaused:
      return "paused";
    case tasks::Task::State::kCompleted:
      return "completed";
    case tasks::Task::State::kCancelled:
      return "cancelled";
    case tasks::Task::State::kFailed:
      return "failed";
  }
  return "unknown";
}

TaskInputSource TaskInputSourceFromString(const std::string& value) {
  if (value == "grpc") return TaskInputSource::kGrpc;
  if (value == "http") return TaskInputSource::kHttp;
  if (value == "recovered") return TaskInputSource::kRecovered;
  throw std::invalid_argument("unknown task input source: " + value);
}

TaskEventKind TaskEventKindFromString(const std::string& value) {
  if (value == "accepted") return TaskEventKind::kAccepted;
  if (value == "rejected") return TaskEventKind::kRejected;
  if (value == "data") return TaskEventKind::kData;
  if (value == "eof") return TaskEventKind::kEof;
  if (value == "error") return TaskEventKind::kError;
  if (value == "state_changed") return TaskEventKind::kStateChanged;
  throw std::invalid_argument("unknown task event kind: " + value);
}

TaskPayloadKind TaskPayloadKindFromString(const std::string& value) {
  if (value == "none") return TaskPayloadKind::kNone;
  if (value == "log_chunk") return TaskPayloadKind::kLogChunk;
  if (value == "pcap_packet") return TaskPayloadKind::kPcapPacket;
  if (value == "exec_chunk") return TaskPayloadKind::kExecChunk;
  if (value == "log_eof") return TaskPayloadKind::kLogEof;
  if (value == "pcap_stats") return TaskPayloadKind::kPcapStats;
  if (value == "exec_exit") return TaskPayloadKind::kExecExit;
  throw std::invalid_argument("unknown task payload kind: " + value);
}

tasks::Task::Kind TaskKindFromString(const std::string& value) {
  if (value == "log_filter") return tasks::Task::Kind::kLogFilter;
  if (value == "pcap") return tasks::Task::Kind::kPcap;
  if (value == "exec") return tasks::Task::Kind::kExec;
  throw std::invalid_argument("unknown task kind: " + value);
}

tasks::Task::State TaskStateFromString(const std::string& value) {
  if (value == "pending") return tasks::Task::State::kPending;
  if (value == "running") return tasks::Task::State::kRunning;
  if (value == "paused") return tasks::Task::State::kPaused;
  if (value == "completed") return tasks::Task::State::kCompleted;
  if (value == "cancelled") return tasks::Task::State::kCancelled;
  if (value == "failed") return tasks::Task::State::kFailed;
  throw std::invalid_argument("unknown task state: " + value);
}

std::int64_t ToUnixMillis(std::chrono::system_clock::time_point ts) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(ts.time_since_epoch()).count();
}

std::chrono::system_clock::time_point FromUnixMillis(std::int64_t value) {
  return std::chrono::system_clock::time_point{std::chrono::milliseconds{value}};
}

}  // namespace zurg::agent::runtime
