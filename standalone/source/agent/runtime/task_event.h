#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "os.pb.h"
#include "tasks/task.h"

namespace zurg::agent::runtime {

enum class TaskInputSource { kGrpc, kHttp, kRecovered };

enum class TaskEventKind { kAccepted, kRejected, kData, kEof, kError, kStateChanged };

enum class TaskPayloadKind {
  kNone,
  kLogChunk,
  kPcapPacket,
  kExecChunk,
  kLogEof,
  kPcapStats,
  kExecExit,
};

struct PayloadRef {
  std::filesystem::path path;
  std::uint64_t size_bytes = 0;
};

struct TaskEvent {
  std::uint64_t sequence = 0;
  std::uint32_t op_id = 0;
  tasks::Task::Kind task_kind = tasks::Task::Kind::kLogFilter;
  TaskInputSource source = TaskInputSource::kGrpc;
  TaskEventKind event_kind = TaskEventKind::kStateChanged;
  TaskPayloadKind payload_kind = TaskPayloadKind::kNone;
  tasks::Task::State state = tasks::Task::State::kPending;
  bool accepted = false;
  std::string code;
  std::string message;
  std::string payload_bytes;
  std::optional<PayloadRef> payload;
  std::chrono::system_clock::time_point timestamp = std::chrono::system_clock::now();
};

const char* ToString(TaskInputSource source);
const char* ToString(TaskEventKind kind);
const char* ToString(TaskPayloadKind kind);
const char* ToString(tasks::Task::Kind kind);
const char* ToString(tasks::Task::State state);

TaskInputSource TaskInputSourceFromString(const std::string& value);
TaskEventKind TaskEventKindFromString(const std::string& value);
TaskPayloadKind TaskPayloadKindFromString(const std::string& value);
tasks::Task::Kind TaskKindFromString(const std::string& value);
tasks::Task::State TaskStateFromString(const std::string& value);

std::int64_t ToUnixMillis(std::chrono::system_clock::time_point ts);
std::chrono::system_clock::time_point FromUnixMillis(std::int64_t value);

}  // namespace zurg::agent::runtime
