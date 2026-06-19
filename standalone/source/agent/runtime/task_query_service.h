#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "runtime/task_store.h"

namespace zurg::agent::runtime {

class TaskQueryService {
 public:
  explicit TaskQueryService(std::shared_ptr<TaskStore> store);

  std::vector<StoredTask> ListTasks() const;
  std::optional<StoredTask> GetTask(std::uint32_t op_id) const;
  std::vector<TaskEvent> ListEvents(std::uint32_t op_id) const;

 private:
  std::shared_ptr<TaskStore> store_;
};

}  // namespace zurg::agent::runtime
