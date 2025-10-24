#pragma once

#include "tasks/task.h"
#include "zurg/log_ops.h"

#include <cstdint>

namespace zurg::agent::tasks {

class LogFilterTask : public Task {
 public:
  LogFilterTask(std::uint32_t op_id,
                const ops::v1::LogFilterSpec& spec,
                const zurg::log_ops::Options& options,
                std::shared_ptr<spdlog::logger> logger);

  void Run(TaskContext& ctx) override;

 private:
  ops::v1::LogFilterSpec spec_;
  zurg::log_ops::Options options_;
};

using LogFilterTaskPtr = std::shared_ptr<LogFilterTask>;

}  // namespace zurg::agent::tasks
