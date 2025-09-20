#include "zurg/pcap_ops.h"

#include <chrono>
#include <random>

#include "google/protobuf/timestamp.pb.h"

namespace zurg::pcap_ops {
namespace {

std::chrono::steady_clock::duration ExtractDuration(const ops::v1::PcapSpec& spec) {
  if (!spec.has_duration()) {
    return std::chrono::steady_clock::duration::zero();
  }
  const auto& d = spec.duration();
  return std::chrono::seconds(d.seconds()) + std::chrono::nanoseconds(d.nanos());
}

}  // namespace

::grpc::Status GenerateCapture(const ops::v1::PcapSpec& spec,
                               CaptureResult* result) {
  if (!result) {
    return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "missing result");
  }
  result->packets.clear();
  result->stats.Clear();

  std::mt19937 rng{std::random_device{}()};
  std::uniform_int_distribution<int> len_dist(60, spec.snaplen() ? spec.snaplen() : 256);
  uint32_t trim = spec.payload_trim_bytes();
  uint64_t limit = spec.packet_limit();
  auto deadline = ExtractDuration(spec);
  auto start = std::chrono::steady_clock::now();

  uint64_t produced = 0;
  while (true) {
    if (limit && produced >= limit) break;
    if (deadline != std::chrono::steady_clock::duration::zero() &&
        std::chrono::steady_clock::now() - start >= deadline) {
      break;
    }
    ops::v1::PcapPacket pkt;
    int len = len_dist(rng);
    if (trim && trim < static_cast<uint32_t>(len)) {
      len = trim;
    }
    std::string data(static_cast<std::size_t>(len), '\0');
    for (auto& ch : data) ch = static_cast<char>(rng() & 0xff);
    pkt.set_data(std::move(data));
    auto now = std::chrono::system_clock::now();
    auto secs = std::chrono::time_point_cast<std::chrono::seconds>(now);
    auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now - secs);
    pkt.mutable_ts()->set_seconds(secs.time_since_epoch().count());
    pkt.mutable_ts()->set_nanos(static_cast<int32_t>(nanos.count()));
    pkt.set_orig_len(len);
    result->packets.push_back(std::move(pkt));
    ++produced;
  }
  result->stats.set_received(produced);
  result->stats.set_dropped(0);
  result->stats.set_if_dropped(0);
  return ::grpc::Status::OK;
}

}  // namespace zurg::pcap_ops
