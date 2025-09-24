#include "zurg/pcap_ops.h"

#include <chrono>
#include <memory>
#include <random>
#include <string>

#include <pcap/pcap.h>

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

::grpc::Status GenerateSynthetic(const ops::v1::PcapSpec& spec,
                                 const PcapPacketConsumer& on_packet,
                                 ops::v1::PcapStats* stats,
                                 const std::function<bool()>& should_stop) {
  if (!on_packet) {
    return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "missing packet callback");
  }
  if (!stats) {
    return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "missing stats");
  }
  std::mt19937 rng{std::random_device{}()};
  std::uniform_int_distribution<int> len_dist(60, spec.snaplen() ? spec.snaplen() : 256);
  const uint32_t trim = spec.payload_trim_bytes();
  const uint64_t limit = spec.packet_limit() ? spec.packet_limit() : 10;
  const auto deadline = ExtractDuration(spec);
  const auto start = std::chrono::steady_clock::now();

  uint64_t produced = 0;
  while (true) {
    if (should_stop && should_stop()) {
      return ::grpc::Status(::grpc::StatusCode::CANCELLED, "capture cancelled");
    }
    if (limit && produced >= limit) break;
    if (deadline != std::chrono::steady_clock::duration::zero() &&
        std::chrono::steady_clock::now() - start >= deadline) {
      break;
    }
    ops::v1::PcapPacket pkt;
    int len = len_dist(rng);
    if (trim && trim < static_cast<uint32_t>(len)) {
      len = static_cast<int>(trim);
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
    auto status = on_packet(std::move(pkt));
    if (!status.ok()) {
      return status;
    }
    ++produced;
  }
  stats->set_received(produced);
  stats->set_dropped(0);
  stats->set_if_dropped(0);
  return ::grpc::Status::OK;
}

}  // namespace

::grpc::Status StreamCapture(const ops::v1::PcapSpec& spec,
                             const PcapPacketConsumer& on_packet,
                             ops::v1::PcapStats* stats,
                             const std::function<bool()>& should_stop) {
  if (!on_packet) {
    return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "missing packet callback");
  }
  if (!stats) {
    return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "missing stats");
  }
  stats->Clear();

  if (spec.if_name().empty()) {
    return GenerateSynthetic(spec, on_packet, stats, should_stop);
  }

  // GCOVR_EXCL_START: Real interface capture requires system resources and is
  // exercised in integration environments rather than unit tests.
  const int snaplen = spec.snaplen() > 0 ? static_cast<int>(spec.snaplen()) : 65535;
  const int promisc = spec.promisc() ? 1 : 0;
  const uint32_t trim = spec.payload_trim_bytes();
  const uint64_t limit = spec.packet_limit();
  const auto deadline = ExtractDuration(spec);
  const bool has_deadline = deadline != std::chrono::steady_clock::duration::zero();
  const auto deadline_point = has_deadline ? std::chrono::steady_clock::now() + deadline
                                           : std::chrono::steady_clock::time_point{};

  char errbuf[PCAP_ERRBUF_SIZE];
  errbuf[0] = '\0';
  pcap_t* handle = pcap_open_live(spec.if_name().c_str(), snaplen, promisc, 1000 /*ms*/, errbuf);
  if (!handle) {
    return ::grpc::Status(::grpc::StatusCode::FAILED_PRECONDITION,
                          errbuf[0] ? errbuf : "pcap_open_live failed");
  }

#ifdef PCAP_D_INOUT
  switch (spec.direction()) {
    case ops::v1::PcapSpec::INGRESS:
#ifdef PCAP_D_IN
      pcap_setdirection(handle, PCAP_D_IN);
#endif
      break;
    case ops::v1::PcapSpec::EGRESS:
#ifdef PCAP_D_OUT
      pcap_setdirection(handle, PCAP_D_OUT);
#endif
      break;
    case ops::v1::PcapSpec::BOTH:
      pcap_setdirection(handle, PCAP_D_INOUT);
      break;
    default:
      break;
  }
#endif

  if (spec.filter_case() == ops::v1::PcapSpec::kBpf && !spec.bpf().empty()) {
    struct bpf_program program;
    if (pcap_compile(handle, &program, spec.bpf().c_str(), 1, PCAP_NETMASK_UNKNOWN) == -1) {
      std::string msg = pcap_geterr(handle);
      pcap_close(handle);
      return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT,
                            msg.empty() ? "failed to compile filter" : msg);
    }
    if (pcap_setfilter(handle, &program) == -1) {
      std::string msg = pcap_geterr(handle);
      pcap_freecode(&program);
      pcap_close(handle);
      return ::grpc::Status(::grpc::StatusCode::FAILED_PRECONDITION,
                            msg.empty() ? "failed to apply filter" : msg);
    }
    pcap_freecode(&program);
  } else if (spec.filter_case() == ops::v1::PcapSpec::kStructured) {
    pcap_close(handle);
    return ::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED,
                          "structured filters are not implemented");
  }

  uint64_t captured = 0;
  auto handle_guard = std::unique_ptr<pcap_t, decltype(&pcap_close)>(handle, &pcap_close);
  while (true) {
    if (should_stop && should_stop()) {
      return ::grpc::Status(::grpc::StatusCode::CANCELLED, "capture cancelled");
    }
    if (limit && captured >= limit) break;
    if (has_deadline && std::chrono::steady_clock::now() >= deadline_point) break;

    struct pcap_pkthdr* header = nullptr;
    const u_char* data = nullptr;
    int rc = pcap_next_ex(handle_guard.get(), &header, &data);
    if (rc == 0) {
      continue;  // timeout
    }
    if (rc == -2) {
      break;  // EOF
    }
    if (rc == -1) {
      std::string msg = pcap_geterr(handle_guard.get());
      return ::grpc::Status(::grpc::StatusCode::UNKNOWN,
                            msg.empty() ? "pcap_next_ex failed" : msg);
    }

    size_t copy_len = header->caplen;
    if (trim > 0 && trim < copy_len) {
      copy_len = trim;
    }

    ops::v1::PcapPacket pkt;
    pkt.set_data(reinterpret_cast<const char*>(data), copy_len);
    pkt.set_orig_len(header->len);
    pkt.mutable_ts()->set_seconds(header->ts.tv_sec);
    pkt.mutable_ts()->set_nanos(static_cast<int32_t>(header->ts.tv_usec * 1000));
    auto status = on_packet(std::move(pkt));
    if (!status.ok()) {
      return status;
    }
    ++captured;
  }

  struct pcap_stat raw_stats;
  if (pcap_stats(handle_guard.get(), &raw_stats) == 0) {
    stats->set_received(raw_stats.ps_recv);
    stats->set_dropped(raw_stats.ps_drop);
    stats->set_if_dropped(raw_stats.ps_ifdrop);
  } else {
    stats->set_received(captured);
    stats->set_dropped(0);
    stats->set_if_dropped(0);
  }

  return ::grpc::Status::OK;
  // GCOVR_EXCL_STOP
}

::grpc::Status GenerateCapture(const ops::v1::PcapSpec& spec,
                               CaptureResult* result) {
  if (!result) {
    return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "missing result");
  }
  result->packets.clear();
  result->stats.Clear();
  auto consumer = [&](ops::v1::PcapPacket packet) -> ::grpc::Status {
    result->packets.push_back(std::move(packet));
    return ::grpc::Status::OK;
  };
  return StreamCapture(spec, consumer, &result->stats);
}

}  // namespace zurg::pcap_ops
