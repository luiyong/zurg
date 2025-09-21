#include "zurg/pcap_ops.h"

#include <gtest/gtest.h>

namespace {

TEST(PcapOpsTest, RejectsNullResult) {
  ops::v1::PcapSpec spec;
  ::grpc::Status s = zurg::pcap_ops::GenerateCapture(spec, nullptr);
  EXPECT_EQ(s.error_code(), ::grpc::StatusCode::INVALID_ARGUMENT);
}

TEST(PcapOpsTest, ProducesPacketsRespectingLimitAndTrim) {
  ops::v1::PcapSpec spec;
  spec.set_snaplen(512);
  spec.set_packet_limit(3);
  spec.set_payload_trim_bytes(4);

  zurg::pcap_ops::CaptureResult result;
  ::grpc::Status s = zurg::pcap_ops::GenerateCapture(spec, &result);
  ASSERT_TRUE(s.ok());
  EXPECT_EQ(result.packets.size(), 3u);
  for (const auto& pkt : result.packets) {
    EXPECT_LE(pkt.data().size(), 4u);
  }
  EXPECT_EQ(result.stats.received(), 3u);
}

TEST(PcapOpsTest, HonorsDurationTimeout) {
  ops::v1::PcapSpec spec;
  spec.mutable_duration()->set_nanos(1);
  spec.set_snaplen(256);
  spec.set_packet_limit(0);

  zurg::pcap_ops::CaptureResult result;
  ::grpc::Status s = zurg::pcap_ops::GenerateCapture(spec, &result);
  ASSERT_TRUE(s.ok());
  EXPECT_LE(result.packets.size(), 1u);
}

}  // namespace
