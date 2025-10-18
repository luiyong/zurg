#include "tasks/pcap_task.h"

#include <utility>

#include "zurg/pcap_ops.h"

namespace zurg::agent::tasks {

PcapTask::PcapTask(const std::string& op_id,
                   const ops::v1::PcapSpec& spec,
                   std::shared_ptr<spdlog::logger> logger)
    : Task(op_id, Kind::kPcap, std::move(logger)), spec_(spec) {}

void PcapTask::Run(TaskContext& ctx) {
  SetState(State::kRunning);
  ops::v1::PcapStats stats;
  auto consumer = [this, &ctx](ops::v1::PcapPacket pkt) -> ::grpc::Status {
    if (CancelRequested()) {
      return ::grpc::Status(::grpc::StatusCode::CANCELLED, "cancelled");
    }
    ctx.SendPcapData(op_id(), std::move(pkt));
    return ::grpc::Status::OK;
  };
  auto should_stop = [this, &ctx]() {
    return CancelRequested() || !ctx.ShouldContinue();
  };

  ::grpc::Status status = zurg::pcap_ops::StreamCapture(spec_, consumer, &stats, should_stop);
  if (status.ok()) {
    SetState(State::kCompleted);
    ctx.SendEofPcap(op_id(), stats);
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

