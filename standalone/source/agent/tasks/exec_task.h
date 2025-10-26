#pragma once

#include "tasks/task.h"

#include <cstdint>

namespace zurg::agent::tasks {

class ExecTask : public Task {
 public:
  ExecTask(std::uint32_t op_id,
           const ops::v1::ExecSpec& spec,
           std::shared_ptr<spdlog::logger> logger);

  bool Validate(std::string* reason) const override;
  void Run(TaskContext& ctx) override;

 private:
  ops::v1::ExecSpec spec_;
};

using ExecTaskPtr = std::shared_ptr<ExecTask>;

}  // namespace zurg::agent::tasks
