#include "tasks/log_filter_task.h"

#include <utility>

namespace zurg::agent::tasks {

LogFilterTask::LogFilterTask(std::uint32_t op_id,
                             const ops::v1::LogFilterSpec& spec,
                             const zurg::log_ops::Options& options,
                             std::shared_ptr<spdlog::logger> logger)
    : Task(op_id, Kind::kLogFilter, std::move(logger)),
      spec_(spec),
      options_(options) {}

void LogFilterTask::Run(TaskContext& ctx) {
  SetState(State::kRunning);
  ops::v1::LogFilterEof eof;
  auto should_stop = [this, &ctx]() {
    return CancelRequested() || !ctx.ShouldContinue();
  };
  auto consumer = [this, &ctx](ops::v1::LogChunk chunk) -> ::grpc::Status {
    if (CancelRequested()) {
      return ::grpc::Status(::grpc::StatusCode::CANCELLED, "cancelled");
    }
    ctx.SendLogData(op_id(), std::move(chunk));
    return ::grpc::Status::OK;
  };

  ::grpc::Status status = zurg::log_ops::StreamLogFilter(options_, spec_, should_stop, consumer, &eof);
  if (status.ok()) {
    SetState(State::kCompleted);
    ctx.SendEofLog(op_id(), eof);
  } else {
    if (status.error_code() == ::grpc::StatusCode::CANCELLED || CancelRequested()) {
      SetState(State::kCancelled);
    } else {
      SetState(State::kFailed);
    }
    ctx.SendError(op_id(), std::to_string(static_cast<int>(status.error_code())), status.error_message());
  }
}

}  // namespace zurg::agent::tasks
