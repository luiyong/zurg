#include "runtime/grpc_task_sink.h"

#include <utility>

namespace zurg::agent::runtime {
namespace {

template <typename T>
bool ParsePayload(const TaskEvent& event, T* out) {
  return out && !event.payload_bytes.empty() && out->ParseFromString(event.payload_bytes);
}

}  // namespace

GrpcTaskSink::GrpcTaskSink(SendFn send_fn) : send_fn_(std::move(send_fn)) {}

void GrpcTaskSink::OnTaskEvent(const TaskEvent& event) {
  if (!send_fn_) {
    return;
  }

  ops::v1::AgentToServer msg;
  switch (event.event_kind) {
    case TaskEventKind::kAccepted:
    case TaskEventKind::kRejected: {
      auto* ack = msg.mutable_ack();
      ack->set_op_id(event.op_id);
      ack->set_accepted(event.accepted);
      ack->set_reason(event.message);
      send_fn_(std::move(msg));
      return;
    }
    case TaskEventKind::kData: {
      auto* data = msg.mutable_data();
      data->set_op_id(event.op_id);
      if (event.payload_kind == TaskPayloadKind::kLogChunk) {
        ops::v1::LogChunk payload;
        if (ParsePayload(event, &payload)) {
          data->mutable_log_chunk()->Swap(&payload);
        }
      } else if (event.payload_kind == TaskPayloadKind::kPcapPacket) {
        ops::v1::PcapPacket payload;
        if (ParsePayload(event, &payload)) {
          data->mutable_pcap_packet()->Swap(&payload);
        }
      } else if (event.payload_kind == TaskPayloadKind::kExecChunk) {
        ops::v1::ExecChunk payload;
        if (ParsePayload(event, &payload)) {
          data->mutable_exec_chunk()->Swap(&payload);
        }
      }
      send_fn_(std::move(msg));
      return;
    }
    case TaskEventKind::kEof: {
      auto* eof = msg.mutable_eof();
      eof->set_op_id(event.op_id);
      if (event.payload_kind == TaskPayloadKind::kLogEof) {
        ops::v1::LogFilterEof payload;
        if (ParsePayload(event, &payload)) {
          eof->mutable_log()->Swap(&payload);
        }
      } else if (event.payload_kind == TaskPayloadKind::kPcapStats) {
        ops::v1::PcapStats payload;
        if (ParsePayload(event, &payload)) {
          eof->mutable_pcap()->Swap(&payload);
        }
      } else if (event.payload_kind == TaskPayloadKind::kExecExit) {
        ops::v1::ExecExit payload;
        if (ParsePayload(event, &payload)) {
          eof->mutable_exec()->Swap(&payload);
        }
      }
      send_fn_(std::move(msg));
      return;
    }
    case TaskEventKind::kError: {
      auto* error = msg.mutable_error();
      error->set_op_id(event.op_id);
      error->set_code(event.code);
      error->set_message(event.message);
      send_fn_(std::move(msg));
      return;
    }
    case TaskEventKind::kStateChanged:
      return;
  }
}

}  // namespace zurg::agent::runtime
