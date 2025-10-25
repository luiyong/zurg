#include "tasks/log_filter_task.h"

#include <utility>

#include <string>

namespace zurg::agent::tasks {

LogFilterTask::LogFilterTask(std::uint32_t op_id,
                             const ops::v1::LogFilterSpec& spec,
                             const zurg::log_ops::Options& options,
                             std::shared_ptr<spdlog::logger> logger)
    : Task(op_id, Kind::kLogFilter, std::move(logger)),
      spec_(spec),
      options_(options) {}

bool LogFilterTask::Validate(std::string* reason) const {
  if (options_.base_path.empty()) {
    if (reason) *reason = "missing log base path";
    return false;
  }
  if (options_.include_rotations && options_.rotation_scan_depth == 0) {
    if (reason) *reason = "invalid rotation depth";
    return false;
  }
  if (options_.chunk_size == 0) {
    if (reason) *reason = "invalid chunk size";
    return false;
  }
  return true;
}

void LogFilterTask::Run(TaskContext& ctx) {
  SetState(State::kRunning);
  ops::v1::LogFilterEof eof;
  auto consumer = [this, &ctx](ops::v1::LogChunk chunk) -> ::grpc::Status {
    ctx.SendLogData(op_id(), std::move(chunk));
    return ::grpc::Status::OK;
  };

  ::grpc::Status status = zurg::log_ops::StreamLogFilter(options_, spec_, consumer, &eof);
  if (status.ok()) {
    SetState(State::kCompleted);
    ctx.SendEofLog(op_id(), eof);
  } else {
    SetState(State::kFailed);
    ctx.SendError(op_id(), std::to_string(static_cast<int>(status.error_code())), status.error_message());
  }
}

}  // namespace zurg::agent::tasks
