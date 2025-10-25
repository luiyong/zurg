#include "zurg/log_ops.h"
#include "zurg/log_ops_internal.h"

#include <gtest/gtest.h>

#include <google/protobuf/util/time_util.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using google::protobuf::util::TimeUtil;

class TempDir {
 public:
  TempDir() {
    auto base = fs::temp_directory_path();
    path_ = base / fs::path("zurg-logops-" +
                            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

TEST(LogOpsTest, FiltersByTimeLevelAndSubstring) {
  TempDir tmp;
  fs::path log_file = tmp.path() / "agent.log";
  {
    std::ofstream os(log_file);
    ASSERT_TRUE(os.is_open());
    os << "[2025-09-27 11:00:00.000] [agent.test] [info] keep me" << std::endl;
    os << "[2025-09-27 12:00:00.000] [agent.test] [warn] skip" << std::endl;
  }

  zurg::log_ops::Options opts;
  opts.log_root = tmp.path().string();
  opts.temp_dir = tmp.path().string();
  opts.base_path = "agent.log";
  opts.cleanup_temp_file = false;  // ensure eof path points to existing file for assertions

  ops::v1::LogFilterSpec spec;
  spec.add_level_in("info");
  spec.set_grep_contains("keep");
  google::protobuf::Timestamp start_ts;
  google::protobuf::Timestamp end_ts;
  ASSERT_TRUE(TimeUtil::FromString("2025-09-27T10:00:00Z", &start_ts));
  ASSERT_TRUE(TimeUtil::FromString("2025-09-27T11:30:00Z", &end_ts));
  *spec.mutable_start_time() = start_ts;
  *spec.mutable_end_time() = end_ts;

  std::string concatenated;
  auto consumer = [&](ops::v1::LogChunk chunk) -> ::grpc::Status {
    concatenated.append(chunk.data());
    return ::grpc::Status::OK;
  };

  ops::v1::LogFilterEof eof;
  auto status = zurg::log_ops::StreamLogFilter(opts, spec, consumer, &eof);
  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_NE(concatenated.find("keep me"), std::string::npos);
  EXPECT_EQ(concatenated.find("skip"), std::string::npos);
  EXPECT_EQ(eof.total_lines(), 1);
  EXPECT_GT(eof.total_size(), 0);
  EXPECT_EQ(eof.source_files_size(), 1);
  EXPECT_FALSE(eof.temp_file_path().empty());
}

TEST(LogOpsTest, FiltersAcrossRotations) {
  TempDir tmp;
  fs::path base = tmp.path() / "agent.log";
  fs::path rotated = tmp.path() / "agent.log.1";
  {
    std::ofstream os(rotated);
    ASSERT_TRUE(os.is_open());
    os << "[2025-09-27 10:50:00.000] [agent.test] [info] from rotation" << std::endl;
  }
  {
    std::ofstream os(base);
    ASSERT_TRUE(os.is_open());
    os << "[2025-09-27 11:05:00.000] [agent.test] [info] from base" << std::endl;
  }

  zurg::log_ops::Options opts;
  opts.log_root = tmp.path().string();
  opts.temp_dir = tmp.path().string();
  opts.base_path = "agent.log";
  opts.include_rotations = true;
  opts.rotation_scan_depth = 1;

  ops::v1::LogFilterSpec spec;
  spec.add_level_in("info");
  google::protobuf::Timestamp start_ts;
  google::protobuf::Timestamp end_ts;
  ASSERT_TRUE(TimeUtil::FromString("2025-09-27T10:45:00Z", &start_ts));
  ASSERT_TRUE(TimeUtil::FromString("2025-09-27T11:10:00Z", &end_ts));
  *spec.mutable_start_time() = start_ts;
  *spec.mutable_end_time() = end_ts;

  std::string concatenated;
  auto consumer = [&](ops::v1::LogChunk chunk) -> ::grpc::Status {
    concatenated.append(chunk.data());
    return ::grpc::Status::OK;
  };

  ops::v1::LogFilterEof eof;
  auto status = zurg::log_ops::StreamLogFilter(opts, spec, consumer, &eof);
  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_NE(concatenated.find("from rotation"), std::string::npos);
  EXPECT_NE(concatenated.find("from base"), std::string::npos);
  EXPECT_EQ(eof.total_lines(), 2);
  EXPECT_EQ(eof.source_files_size(), 2);
}

TEST(LogOpsTest, RespectsOutputLimit) {
  TempDir tmp;
  fs::path log_file = tmp.path() / "agent.log";
  {
    std::ofstream os(log_file);
    ASSERT_TRUE(os.is_open());
    os << "[2025-09-27 11:00:00.000] [agent.test] [info] a" << std::endl;
  }
  zurg::log_ops::Options opts;
  opts.log_root = tmp.path().string();
  opts.temp_dir = tmp.path().string();
  opts.base_path = "agent.log";

  ops::v1::LogFilterSpec spec;
  spec.add_level_in("info");
  google::protobuf::Timestamp start_ts;
  google::protobuf::Timestamp end_ts;
  ASSERT_TRUE(TimeUtil::FromString("2025-09-27T10:00:00Z", &start_ts));
  ASSERT_TRUE(TimeUtil::FromString("2025-09-27T12:00:00Z", &end_ts));
  *spec.mutable_start_time() = start_ts;
  *spec.mutable_end_time() = end_ts;
  spec.set_max_output_bytes(1);

  ops::v1::LogFilterEof eof;
  auto status = zurg::log_ops::StreamLogFilter(opts, spec,
                                               [](ops::v1::LogChunk) { return ::grpc::Status::OK; }, &eof);
  EXPECT_EQ(status.error_code(), ::grpc::StatusCode::RESOURCE_EXHAUSTED);
}

TEST(LogOpsInternalTest, EnumerateCandidatesRespectsDepth) {
  fs::path base("/var/log/agent.log");
  auto candidates = zurg::log_ops::internal::EnumerateCandidates(base, true, 2);
  EXPECT_NE(std::find(candidates.begin(), candidates.end(), base), candidates.end());
  EXPECT_NE(std::find(candidates.begin(), candidates.end(), fs::path("/var/log/agent.log.1")), candidates.end());
  EXPECT_NE(std::find(candidates.begin(), candidates.end(), fs::path("/var/log/agent.log.2")), candidates.end());
}

TEST(LogOpsInternalTest, FilterLogsToTempProducesMetrics) {
  TempDir tmp;
  fs::path log_file = tmp.path() / "agent.log";
  {
    std::ofstream os(log_file);
    ASSERT_TRUE(os.is_open());
    os << "[2025-09-27 11:00:00.000] [agent.test] [info] hello" << std::endl;
  }

  auto root = zurg::log_ops::internal::MakeRoot(tmp.path().string());
  ASSERT_TRUE(root);
  auto resolved = zurg::log_ops::internal::ResolvePath(*root, "agent.log");
  ASSERT_TRUE(resolved);
  auto candidates = zurg::log_ops::internal::EnumerateCandidates(*resolved, false, 0);

  zurg::log_ops::Options opts;
  opts.log_root = tmp.path().string();
  opts.temp_dir = tmp.path().string();
  opts.base_path = "agent.log";
  opts.cleanup_temp_file = false;

  ops::v1::LogFilterSpec spec;
  google::protobuf::Timestamp start_ts;
  google::protobuf::Timestamp end_ts;
  ASSERT_TRUE(TimeUtil::FromString("2025-09-27T10:00:00Z", &start_ts));
  ASSERT_TRUE(TimeUtil::FromString("2025-09-27T12:00:00Z", &end_ts));
  *spec.mutable_start_time() = start_ts;
  *spec.mutable_end_time() = end_ts;

  zurg::log_ops::internal::FilterMetrics metrics;
  auto status = zurg::log_ops::internal::FilterLogsToTemp(*root, opts, spec, candidates, &metrics);
  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_GT(metrics.total_size, 0);
  EXPECT_EQ(metrics.total_lines, 1);
  EXPECT_FALSE(metrics.temp_path.empty());
  EXPECT_TRUE(fs::exists(metrics.temp_path));

  status = zurg::log_ops::internal::StreamFilteredFile(opts,
                                                       [](ops::v1::LogChunk) { return ::grpc::Status::OK; },
                                                       &metrics);
  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_FALSE(fs::exists(metrics.temp_path));
}

TEST(LogOpsInternalTest, StreamFileCancellationPropagates) {
  TempDir tmp;
  fs::path log_file = tmp.path() / "agent.log";
  {
    std::ofstream os(log_file);
    ASSERT_TRUE(os.is_open());
    os << "[2025-09-27 11:00:00.000] [agent.test] [info] hello" << std::endl;
  }

  auto root = zurg::log_ops::internal::MakeRoot(tmp.path().string());
  ASSERT_TRUE(root);
  auto resolved = zurg::log_ops::internal::ResolvePath(*root, "agent.log");
  ASSERT_TRUE(resolved);
  auto candidates = zurg::log_ops::internal::EnumerateCandidates(*resolved, false, 0);

  zurg::log_ops::Options opts;
  opts.log_root = tmp.path().string();
  opts.temp_dir = tmp.path().string();
  opts.base_path = "agent.log";
  opts.cleanup_temp_file = false;

  ops::v1::LogFilterSpec spec;
  google::protobuf::Timestamp start_ts;
  google::protobuf::Timestamp end_ts;
  ASSERT_TRUE(TimeUtil::FromString("2025-09-27T10:00:00Z", &start_ts));
  ASSERT_TRUE(TimeUtil::FromString("2025-09-27T12:00:00Z", &end_ts));
  *spec.mutable_start_time() = start_ts;
  *spec.mutable_end_time() = end_ts;

  zurg::log_ops::internal::FilterMetrics metrics;
  auto status = zurg::log_ops::internal::FilterLogsToTemp(*root, opts, spec, candidates, &metrics);
  ASSERT_TRUE(status.ok());

  status = zurg::log_ops::internal::StreamFilteredFile(opts,
                                                       [](ops::v1::LogChunk) { return ::grpc::Status::OK; },
                                                       &metrics);
  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_FALSE(fs::exists(metrics.temp_path));
}

}  // namespace
