#include "tasks/pcap_task.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "zurg/pcap_ops.h"
#include "zurg/temp_file.h"

namespace zurg::agent::tasks {
namespace {
namespace fs = std::filesystem;

void WriteGlobalHeader(std::ofstream& out, uint32_t snaplen, uint32_t network) {
  struct Header {
    uint32_t magic = 0xa1b2c3d4;
    uint16_t major = 2;
    uint16_t minor = 4;
    int32_t thiszone = 0;
    uint32_t sigfigs = 0;
    uint32_t snaplen;
    uint32_t network;
  } header{0xa1b2c3d4, 2, 4, 0, 0, snaplen, network};
  out.write(reinterpret_cast<const char*>(&header), sizeof(header));
}

void WriteRecord(std::ofstream& out, const ops::v1::PcapPacket& pkt) {
  struct RecordHeader {
    uint32_t ts_sec;
    uint32_t ts_usec;
    uint32_t incl_len;
    uint32_t orig_len;
  } rec{};
  rec.ts_sec = static_cast<uint32_t>(pkt.ts().seconds());
  rec.ts_usec = static_cast<uint32_t>(pkt.ts().nanos() / 1000);
  rec.incl_len = static_cast<uint32_t>(pkt.data().size());
  rec.orig_len = static_cast<uint32_t>(pkt.orig_len());
  out.write(reinterpret_cast<const char*>(&rec), sizeof(rec));
  if (!pkt.data().empty()) {
    out.write(pkt.data().data(), static_cast<std::streamsize>(pkt.data().size()));
  }
}

}  // namespace

PcapTask::PcapTask(std::uint32_t op_id,
                   const ops::v1::PcapSpec& spec,
                   const zurg::log_ops::Options& file_options,
                   std::shared_ptr<spdlog::logger> logger)
    : Task(op_id, Kind::kPcap, std::move(logger)), spec_(spec), file_options_(file_options) {}

void PcapTask::Run(TaskContext& ctx) {
  SetState(State::kRunning);

  fs::path temp_dir;
  try {
    temp_dir = zurg::temp_file::ResolveDirectory(file_options_.temp_dir);
  } catch (const std::filesystem::filesystem_error& err) {
    SetState(State::kFailed);
    ctx.SendError(op_id(), "INTERNAL", err.code().message());
    return;
  }
  fs::path temp_file;
  try {
    temp_file = zurg::temp_file::CreateUniquePath(temp_dir,
                                                  "pcap-" + std::to_string(op_id()) + "-", ".pcap");
  } catch (const std::exception& ex) {
    SetState(State::kFailed);
    ctx.SendError(op_id(), "INTERNAL", ex.what());
    return;
  }

  std::ofstream out(temp_file, std::ios::binary | std::ios::trunc);
  if (!out) {
    SetState(State::kFailed);
    ctx.SendError(op_id(), "INTERNAL", "failed to create temp pcap file");
    return;
  }

  const uint32_t snaplen = spec_.snaplen() > 0 ? static_cast<uint32_t>(spec_.snaplen()) : 65535;
  bool header_written = false;
  uint32_t network = 1;
  auto ensure_header = [&](uint32_t linktype) {
    if (!header_written) {
      network = linktype;
      WriteGlobalHeader(out, snaplen, network);
      header_written = true;
    }
  };

  auto should_stop = [this, &ctx]() {
    return CancelRequested() || !ctx.ShouldContinue();
  };

  ops::v1::PcapStats stats;
  int datalink = 1;
  auto consumer = [&](ops::v1::PcapPacket pkt) -> ::grpc::Status {
    if (CancelRequested()) {
      return ::grpc::Status(::grpc::StatusCode::CANCELLED, "cancelled");
    }
    ensure_header(static_cast<uint32_t>(datalink));
    WriteRecord(out, pkt);
    if (!out) {
      return ::grpc::Status(::grpc::StatusCode::INTERNAL, "failed to write pcap record");
    }
    return ::grpc::Status::OK;
  };

  ::grpc::Status status = zurg::pcap_ops::StreamCapture(spec_, consumer, &stats, should_stop, &datalink);

  if (status.ok() && !header_written) {
    ensure_header(static_cast<uint32_t>(datalink));
  }

  out.close();

  auto cleanup = [&]() {
    if (file_options_.cleanup_temp_file) {
      std::error_code del_ec;
      fs::remove(temp_file, del_ec);
    }
  };

  if (!status.ok()) {
    cleanup();
    if (status.error_code() == ::grpc::StatusCode::CANCELLED || CancelRequested()) {
      SetState(State::kCancelled);
    } else {
      SetState(State::kFailed);
    }
    ctx.SendError(op_id(), std::to_string(static_cast<int>(status.error_code())), status.error_message());
    return;
  }

  const std::size_t chunk_size = file_options_.chunk_size == 0 ? 64 * 1024 : file_options_.chunk_size;
  auto stream_status = zurg::temp_file::StreamFile(
      temp_file, chunk_size,
      [&]() { return CancelRequested() || !ctx.ShouldContinue(); },
      [&](std::int64_t offset, std::string_view data) -> ::grpc::Status {
        ops::v1::LogChunk chunk;
        chunk.set_offset(offset);
        chunk.set_data(data.data(), data.size());
        ctx.SendLogData(op_id(), std::move(chunk));
        return ::grpc::Status::OK;
      });

  cleanup();

  if (!stream_status.ok()) {
    if (stream_status.error_code() == ::grpc::StatusCode::CANCELLED) {
      SetState(State::kCancelled);
      ctx.SendError(op_id(), "CANCELLED", stream_status.error_message());
    } else {
      SetState(State::kFailed);
      ctx.SendError(op_id(), std::to_string(static_cast<int>(stream_status.error_code())),
                    stream_status.error_message());
    }
    return;
  }

  SetState(State::kCompleted);
  ctx.SendEofPcap(op_id(), stats);
}

}  // namespace zurg::agent::tasks
