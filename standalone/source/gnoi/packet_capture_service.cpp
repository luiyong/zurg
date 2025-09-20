#include "zurg/gnoi/packet_capture_service.h"

#include "zurg/pcap_ops.h"

namespace zurg::gnoi_pcap {
namespace {

class PacketCaptureServiceImpl final : public gnoi::pcap::PacketCapture::Service {
 public:
  ::grpc::Status Pcap(::grpc::ServerContext* ctx, const gnoi::pcap::PcapRequest* request,
                      ::grpc::ServerWriter<gnoi::pcap::PcapResponse>* writer) override {
    ops::v1::PcapSpec spec;
    spec.set_snaplen(256);
    uint32_t packet_limit = 10;
    if (request) {
      if (request->packet_count() > 0) {
        packet_limit = request->packet_count();
      }
      if (request->trim_payload() > 0) {
        spec.set_payload_trim_bytes(request->trim_payload());
      }
      if (request->duration() > 0) {
        auto total_nanos = request->duration();
        auto secs = static_cast<int64_t>(total_nanos / 1000000000ULL);
        auto nanos = static_cast<int32_t>(total_nanos % 1000000000ULL);
        spec.mutable_duration()->set_seconds(secs);
        spec.mutable_duration()->set_nanos(nanos);
      }
    }
    spec.set_packet_limit(packet_limit);

    zurg::pcap_ops::CaptureResult result;
    auto status = zurg::pcap_ops::GenerateCapture(spec, &result);
    if (!status.ok()) {
      return status;
    }

    for (const auto& pkt : result.packets) {
      if (ctx && ctx->IsCancelled()) {
        return ::grpc::Status(::grpc::StatusCode::CANCELLED, "cancelled");
      }
      gnoi::pcap::PcapResponse resp;
      auto* out = resp.add_packets();
      out->set_data(pkt.data());
      if (!writer->Write(resp)) {
        if (ctx && ctx->IsCancelled()) {
          return ::grpc::Status(::grpc::StatusCode::CANCELLED, "cancelled");
        }
        break;
      }
    }

    return ::grpc::Status::OK;
  }
};

}  // namespace

std::unique_ptr<gnoi::pcap::PacketCapture::Service> CreatePacketCaptureService() {
  return std::make_unique<PacketCaptureServiceImpl>();
}

}  // namespace zurg::gnoi_pcap
