#include "runtime/task_query_service.h"

namespace zurg::agent::runtime {

TaskQueryService::TaskQueryService(std::shared_ptr<TaskStore> store) : store_(std::move(store)) {}

std::vector<StoredTask> TaskQueryService::ListTasks() const {
  if (!store_) {
    return {};
  }
  return store_->ListTasks();
}

std::optional<StoredTask> TaskQueryService::GetTask(std::uint32_t op_id) const {
  if (!store_) {
    return std::nullopt;
  }
  return store_->GetTask(op_id);
}

std::vector<TaskEvent> TaskQueryService::ListEvents(std::uint32_t op_id) const {
  if (!store_) {
    return {};
  }
  return store_->ListEvents(op_id);
}

}  // namespace zurg::agent::runtime
