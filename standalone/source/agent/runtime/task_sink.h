#pragma once

#include "runtime/task_event.h"

namespace zurg::agent::runtime {

class TaskSink {
 public:
  virtual ~TaskSink() = default;
  virtual void OnTaskEvent(const TaskEvent& event) = 0;
};

}  // namespace zurg::agent::runtime
