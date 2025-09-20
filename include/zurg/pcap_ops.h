#pragma once

#include <grpcpp/grpcpp.h>

#include <vector>

#include "os.pb.h"

namespace zurg::pcap_ops {

struct CaptureResult {
  std::vector<ops::v1::PcapPacket> packets;
  ops::v1::PcapStats stats;
};

::grpc::Status GenerateCapture(const ops::v1::PcapSpec& spec,
                               CaptureResult* result);

}  // namespace zurg::pcap_ops
