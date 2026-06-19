#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "runtime/task_event.h"
#include "runtime/task_sink.h"

namespace zurg::agent::runtime {

struct StoredTask {
  std::uint32_t op_id = 0;
  tasks::Task::Kind task_kind = tasks::Task::Kind::kLogFilter;
  TaskInputSource source = TaskInputSource::kRecovered;
  tasks::Task::State state = tasks::Task::State::kPending;
  std::int64_t created_at_ms = 0;
  std::int64_t updated_at_ms = 0;
  std::string error_code;
  std::string error_message;
};

class TaskStore : public TaskSink {
 public:
  ~TaskStore() override = default;

  virtual bool Open(std::string* error) = 0;
  virtual std::vector<StoredTask> ListTasks() const = 0;
  virtual std::optional<StoredTask> GetTask(std::uint32_t op_id) const = 0;
  virtual std::vector<TaskEvent> ListEvents(std::uint32_t op_id) const = 0;
};

}  // namespace zurg::agent::runtime
