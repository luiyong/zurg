#include "zurg/pcap_ops.h"

#include <gtest/gtest.h>

#include <chrono>
#include <vector>

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

TEST(PcapOpsTest, StreamCaptureRespectsPacketLimit) {
  ops::v1::PcapSpec spec;
  spec.set_packet_limit(100);
  spec.set_snaplen(256);

  std::size_t count = 0;
  auto consumer = [&](ops::v1::PcapPacket pkt) -> ::grpc::Status {
    ++count;
    return ::grpc::Status::OK;
  };

  ops::v1::PcapStats stats;
  ::grpc::Status status = zurg::pcap_ops::StreamCapture(spec, consumer, &stats);
  ASSERT_TRUE(status.ok());
  EXPECT_EQ(count, 100u);
  EXPECT_EQ(stats.received(), 100u);
}

TEST(PcapOpsTest, StreamCaptureRespectsDuration) {
  ops::v1::PcapSpec spec;
  spec.mutable_duration()->set_seconds(10);
  spec.set_packet_limit(0);
  spec.set_snaplen(128);

  std::vector<std::chrono::steady_clock::time_point> timeline;
  auto base = std::chrono::steady_clock::time_point{};
  timeline.push_back(base);
  timeline.push_back(base + std::chrono::seconds(3));
  timeline.push_back(base + std::chrono::seconds(6));
  timeline.push_back(base + std::chrono::seconds(9));
  timeline.push_back(base + std::chrono::seconds(12));

  std::size_t cursor = 0;
  auto now_provider = [&]() {
    if (cursor >= timeline.size()) {
      return timeline.back();
    }
    return timeline[cursor++];
  };

  std::size_t count = 0;
  auto consumer = [&](ops::v1::PcapPacket pkt) -> ::grpc::Status {
    ++count;
    return ::grpc::Status::OK;
  };

  ops::v1::PcapStats stats;
  ::grpc::Status status = zurg::pcap_ops::StreamCapture(spec, consumer, &stats,
                                                        /*should_stop=*/{},
                                                        /*datalink=*/nullptr,
                                                        now_provider);
  ASSERT_TRUE(status.ok());
  EXPECT_GT(count, 0u);
  EXPECT_LE(count, timeline.size());
  EXPECT_EQ(stats.received(), count);
}

}  // namespace
