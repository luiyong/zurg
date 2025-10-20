#pragma once

#include "tasks/task.h"
#include "zurg/log_ops.h"

namespace zurg::agent::tasks {

class PcapTask : public Task {
 public:
  PcapTask(const std::string& op_id,
           const ops::v1::PcapSpec& spec,
           const zurg::log_ops::Options& file_options,
           std::shared_ptr<spdlog::logger> logger);

  void Run(TaskContext& ctx) override;

 private:
  ops::v1::PcapSpec spec_;
  zurg::log_ops::Options file_options_;
};

using PcapTaskPtr = std::shared_ptr<PcapTask>;

}  // namespace zurg::agent::tasks
