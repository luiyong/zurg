#pragma once

#include <grpcpp/grpcpp.h>

#include <functional>
#include <vector>

#include "os.pb.h"

namespace zurg::pcap_ops {

struct CaptureResult {
  std::vector<ops::v1::PcapPacket> packets;
  ops::v1::PcapStats stats;
};

using PcapPacketConsumer = std::function<::grpc::Status(ops::v1::PcapPacket)>;

::grpc::Status StreamCapture(const ops::v1::PcapSpec& spec,
                             const PcapPacketConsumer& on_packet,
                             ops::v1::PcapStats* stats,
                             const std::function<bool()>& should_stop = {});

::grpc::Status GenerateCapture(const ops::v1::PcapSpec& spec,
                               CaptureResult* result);

}  // namespace zurg::pcap_ops
