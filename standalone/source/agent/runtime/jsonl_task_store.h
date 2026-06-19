#pragma once

#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <spdlog/logger.h>

#include "runtime/task_store.h"

namespace zurg::agent::runtime {

class JsonlTaskStore : public TaskStore {
 public:
  explicit JsonlTaskStore(std::filesystem::path root,
                          std::shared_ptr<spdlog::logger> logger = nullptr);

  bool Open(std::string* error) override;
  void OnTaskEvent(const TaskEvent& event) override;

  std::vector<StoredTask> ListTasks() const override;
  std::optional<StoredTask> GetTask(std::uint32_t op_id) const override;
  std::vector<TaskEvent> ListEvents(std::uint32_t op_id) const override;

  const std::filesystem::path& root() const { return root_; }

 private:
  std::filesystem::path PayloadPath(const TaskEvent& event) const;
  void ApplyEventLocked(const TaskEvent& event);
  void MarkInterruptedTasksLocked();

  std::filesystem::path root_;
  std::filesystem::path events_path_;
  std::filesystem::path payload_dir_;
  std::shared_ptr<spdlog::logger> logger_;

  mutable std::mutex mu_;
  std::uint64_t next_sequence_ = 1;
  std::unordered_map<std::uint32_t, StoredTask> tasks_;
  std::unordered_map<std::uint32_t, std::vector<TaskEvent>> events_;
};

}  // namespace zurg::agent::runtime
