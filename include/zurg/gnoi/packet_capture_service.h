#pragma once

#include <grpcpp/grpcpp.h>

#include <memory>

#include "github.com/openconfig/gnoi/packet_capture/packet_capture.grpc.pb.h"

namespace zurg::gnoi_pcap {

std::unique_ptr<gnoi::pcap::PacketCapture::Service> CreatePacketCaptureService();

}  // namespace zurg::gnoi_pcap

