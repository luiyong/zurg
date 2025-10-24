#include "zurg/temp_file.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include <grpcpp/grpcpp.h>

namespace {

std::filesystem::path MakeScratchDir() {
  auto base = std::filesystem::temp_directory_path();
  auto dir = base / std::filesystem::path("zurg-temp-file-test") /
             std::filesystem::path(std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(dir);
  return dir;
}

TEST(TempFileTest, ResolveDirectoryCreatesWhenMissing) {
  auto dir = MakeScratchDir();
  auto nested = dir / "subdir";
  EXPECT_FALSE(std::filesystem::exists(nested));
  auto resolved = zurg::temp_file::ResolveDirectory(nested.string());
  EXPECT_TRUE(std::filesystem::exists(resolved));
  EXPECT_EQ(std::filesystem::canonical(resolved), std::filesystem::canonical(nested));
  std::filesystem::remove_all(dir);
}

TEST(TempFileTest, CreateUniquePathGeneratesDistinctFiles) {
  auto dir = MakeScratchDir();
  auto first = zurg::temp_file::CreateUniquePath(dir, "prefix-", ".tmp");
  auto second = zurg::temp_file::CreateUniquePath(dir, "prefix-", ".tmp");
  EXPECT_NE(first, second);
  EXPECT_EQ(first.parent_path(), dir);
  EXPECT_EQ(second.parent_path(), dir);
  std::filesystem::remove_all(dir);
}

TEST(TempFileTest, StreamFileReadsContentAndHonorsCancellation) {
  auto dir = MakeScratchDir();
  auto path = dir / "data.bin";
  {
    std::ofstream out(path, std::ios::binary);
    ASSERT_TRUE(out.is_open());
    out << "0123456789abcdef";
  }

  std::string collected;
  auto status = zurg::temp_file::StreamFile(
      path, 4,
      [] { return false; },
      [&](std::int64_t offset, std::string_view chunk) {
        collected.append(chunk);
        EXPECT_EQ(static_cast<std::size_t>(offset), collected.size() - chunk.size());
        return ::grpc::Status::OK;
      });
  ASSERT_TRUE(status.ok());
  EXPECT_EQ(collected, "0123456789abcdef");

  std::atomic<bool> stop{false};
  status = zurg::temp_file::StreamFile(
      path, 4,
      [&] {
        bool expected = false;
        if (stop.compare_exchange_strong(expected, true)) {
          return false;
        }
        return true;
      },
      [&](std::int64_t, std::string_view) { return ::grpc::Status::OK; });
  EXPECT_EQ(status.error_code(), ::grpc::StatusCode::CANCELLED);
  std::filesystem::remove_all(dir);
}

}  // namespace
