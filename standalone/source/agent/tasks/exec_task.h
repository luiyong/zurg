#pragma once

#include "tasks/task.h"

namespace zurg::agent::tasks {

class ExecTask : public Task {
 public:
  ExecTask(const std::string& op_id,
           const ops::v1::ExecSpec& spec,
           std::shared_ptr<spdlog::logger> logger);

  void Run(TaskContext& ctx) override;

 private:
  ops::v1::ExecSpec spec_;
};

using ExecTaskPtr = std::shared_ptr<ExecTask>;

}  // namespace zurg::agent::tasks

