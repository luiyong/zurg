#pragma once

#include "tasks/task.h"

namespace zurg::agent::tasks {

class PcapTask : public Task {
 public:
  PcapTask(const std::string& op_id,
           const ops::v1::PcapSpec& spec,
           std::shared_ptr<spdlog::logger> logger);

  void Run(TaskContext& ctx) override;

 private:
  ops::v1::PcapSpec spec_;
};

using PcapTaskPtr = std::shared_ptr<PcapTask>;

}  // namespace zurg::agent::tasks

