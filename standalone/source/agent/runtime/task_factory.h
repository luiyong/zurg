#pragma once

#include <memory>
#include <string>
#include <variant>

#include <spdlog/logger.h>

#include "os.pb.h"
#include "runtime/task_event.h"
#include "tasks/task.h"
#include "zurg/log_ops.h"

namespace zurg::agent::runtime {

struct TaskRequest {
  std::uint32_t op_id = 0;
  TaskInputSource source = TaskInputSource::kGrpc;
  std::variant<ops::v1::LogFilterSpec, ops::v1::PcapSpec, ops::v1::ExecSpec> spec;
};

struct TaskBuildResult {
  std::shared_ptr<tasks::Task> task;
  tasks::Task::Kind kind = tasks::Task::Kind::kLogFilter;
  std::string error;
};

class TaskFactory {
 public:
  TaskFactory(zurg::log_ops::Options log_options, std::shared_ptr<spdlog::logger> logger);

  TaskBuildResult Build(const TaskRequest& request) const;
  TaskBuildResult Build(const ops::v1::StartOp& start, TaskInputSource source) const;

 private:
  zurg::log_ops::Options log_options_;
  std::shared_ptr<spdlog::logger> logger_;
};

}  // namespace zurg::agent::runtime
