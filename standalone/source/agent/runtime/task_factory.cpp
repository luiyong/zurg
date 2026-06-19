#include "runtime/task_factory.h"

#include <utility>

#include "tasks/exec_task.h"
#include "tasks/log_filter_task.h"
#include "tasks/pcap_task.h"

namespace zurg::agent::runtime {

TaskFactory::TaskFactory(zurg::log_ops::Options log_options,
                         std::shared_ptr<spdlog::logger> logger)
    : log_options_(std::move(log_options)), logger_(std::move(logger)) {}

TaskBuildResult TaskFactory::Build(const TaskRequest& request) const {
  TaskBuildResult result;
  if (request.op_id == 0) {
    result.error = "missing op_id";
    return result;
  }

  if (std::holds_alternative<ops::v1::LogFilterSpec>(request.spec)) {
    result.kind = tasks::Task::Kind::kLogFilter;
    result.task = std::make_shared<tasks::LogFilterTask>(
        request.op_id, std::get<ops::v1::LogFilterSpec>(request.spec), log_options_, logger_);
  } else if (std::holds_alternative<ops::v1::PcapSpec>(request.spec)) {
    result.kind = tasks::Task::Kind::kPcap;
    result.task = std::make_shared<tasks::PcapTask>(
        request.op_id, std::get<ops::v1::PcapSpec>(request.spec), log_options_, logger_);
  } else {
    result.kind = tasks::Task::Kind::kExec;
    result.task = std::make_shared<tasks::ExecTask>(
        request.op_id, std::get<ops::v1::ExecSpec>(request.spec), logger_);
  }

  std::string validation_error;
  if (!result.task->Validate(&validation_error)) {
    result.task.reset();
    result.error = validation_error.empty() ? "invalid parameters" : std::move(validation_error);
  }
  return result;
}

TaskBuildResult TaskFactory::Build(const ops::v1::StartOp& start,
                                   TaskInputSource source) const {
  TaskRequest request;
  request.op_id = start.meta().op_id();
  request.source = source;
  if (start.has_log_filter()) {
    request.spec = start.log_filter();
  } else if (start.has_pcap()) {
    request.spec = start.pcap();
  } else if (start.has_exec()) {
    request.spec = start.exec();
  } else {
    TaskBuildResult result;
    result.error = "unsupported operation";
    return result;
  }
  return Build(request);
}

}  // namespace zurg::agent::runtime
