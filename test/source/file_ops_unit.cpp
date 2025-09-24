#include "zurg/file_ops.h"

#include <gtest/gtest.h>

#include <openssl/evp.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace fs = std::filesystem;

struct TempDir {
  fs::path path;
  explicit TempDir(std::string_view name) {
    path = fs::temp_directory_path() / fs::path("zurg_file_ops_" + std::string(name));
    fs::remove_all(path);
    fs::create_directories(path);
  }
  ~TempDir() { fs::remove_all(path); }
};

std::string Sha256(std::string_view data) {
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
  if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1) return {};
  EVP_DigestUpdate(ctx.get(), data.data(), data.size());
  unsigned int len = EVP_MD_size(EVP_sha256());
  std::string out(len, '\0');
  EVP_DigestFinal_ex(ctx.get(), reinterpret_cast<unsigned char*>(out.data()), &len);
  out.resize(len);
  return out;
}

TEST(FileOpsTest, ReadFileCompressionUnsupported) {
  TempDir tmp("compress");
  std::ofstream(tmp.path / "payload.txt") << "payload";

  zurg::file_ops::Options opts{tmp.path.string()};
  ops::v1::FileGetSpec spec;
  spec.set_path("payload.txt");
  spec.set_compress(true);

  zurg::file_ops::FileGetResult result;
  ::grpc::Status s = zurg::file_ops::ReadFile(opts, spec, &result);
  EXPECT_EQ(s.error_code(), ::grpc::StatusCode::UNIMPLEMENTED);
}

TEST(FileOpsTest, ReadFileInvalidOffset) {
  TempDir tmp("offset");
  std::ofstream(tmp.path / "data.bin") << "12345";

  zurg::file_ops::Options opts{tmp.path.string()};
  ops::v1::FileGetSpec spec;
  spec.set_path("data.bin");
  spec.set_offset(99);

  zurg::file_ops::FileGetResult result;
  ::grpc::Status s = zurg::file_ops::ReadFile(opts, spec, &result);
  EXPECT_EQ(s.error_code(), ::grpc::StatusCode::INVALID_ARGUMENT);
}

TEST(FileOpsTest, ReadFileWithExpectedChecksum) {
  TempDir tmp("checksum_read");
  const std::string contents = "checksum-data";
  std::ofstream(tmp.path / "file.txt") << contents;

  zurg::file_ops::Options opts{tmp.path.string()};
  ops::v1::FileGetSpec spec;
  spec.set_path("file.txt");
  auto expect = spec.mutable_expect();
  expect->set_type(ops::v1::Checksum::SHA256);
  expect->set_digest(Sha256(contents));

  zurg::file_ops::FileGetResult result;
  ::grpc::Status s = zurg::file_ops::ReadFile(opts, spec, &result);
  ASSERT_TRUE(s.ok());
  ASSERT_EQ(result.eof.checksum().digest(), expect->digest());
}


TEST(FileOpsTest, ReadFileOutsideRootDenied) {
  TempDir tmp("read_outside");
  std::ofstream(tmp.path / "safe.txt") << "content";

  zurg::file_ops::Options opts{tmp.path.string()};
  ops::v1::FileGetSpec spec;
  spec.set_path("../escape.txt");

  zurg::file_ops::FileGetResult result;
  ::grpc::Status s = zurg::file_ops::ReadFile(opts, spec, &result);
  EXPECT_EQ(s.error_code(), ::grpc::StatusCode::PERMISSION_DENIED);
}

TEST(FileOpsTest, ReadFileMissingSource) {
  TempDir tmp("read_missing");
  zurg::file_ops::Options opts{tmp.path.string()};
  ops::v1::FileGetSpec spec;
  spec.set_path("absent.txt");

  zurg::file_ops::FileGetResult result;
  ::grpc::Status s = zurg::file_ops::ReadFile(opts, spec, &result);
  EXPECT_EQ(s.error_code(), ::grpc::StatusCode::NOT_FOUND);
}

TEST(FileOpsTest, ReadFileComputesChecksumWhenAbsent) {
  TempDir tmp("checksum_auto");
  const std::string contents = "auto-checksum";
  std::ofstream(tmp.path / "auto.txt") << contents;

  zurg::file_ops::Options opts{tmp.path.string()};
  ops::v1::FileGetSpec spec;
  spec.set_path("auto.txt");

  zurg::file_ops::FileGetResult result;
  ::grpc::Status s = zurg::file_ops::ReadFile(opts, spec, &result);
  ASSERT_TRUE(s.ok());
  EXPECT_EQ(result.eof.checksum().type(), ops::v1::Checksum::SHA256);
  EXPECT_FALSE(result.eof.checksum().digest().empty());
}

TEST(FileOpsTest, ReadFileChecksumMismatchFails) {
  TempDir tmp("checksum_mismatch");
  const std::string contents = "mismatch";
  std::ofstream(tmp.path / "mismatch.txt") << contents;

  zurg::file_ops::Options opts{tmp.path.string()};
  ops::v1::FileGetSpec spec;
  spec.set_path("mismatch.txt");
  auto expect = spec.mutable_expect();
  expect->set_type(ops::v1::Checksum::SHA256);
  expect->set_digest(Sha256("something different"));

  zurg::file_ops::FileGetResult result;
  ::grpc::Status s = zurg::file_ops::ReadFile(opts, spec, &result);
  EXPECT_EQ(s.error_code(), ::grpc::StatusCode::ABORTED);
}

TEST(FileOpsTest, WriteFileChecksumMismatch) {
  TempDir tmp("checksum");
  zurg::file_ops::Options opts{tmp.path.string()};

  std::vector<ops::v1::FileChunk> chunks(1);
  chunks[0].set_offset(0);
  chunks[0].set_data("hello world");

  ops::v1::Checksum checksum;
  checksum.set_type(ops::v1::Checksum::SHA256);
  checksum.set_digest(Sha256("something else"));

  ::grpc::Status s = zurg::file_ops::WriteFile(opts, "file.bin", chunks, checksum, 0644);
  EXPECT_EQ(s.error_code(), ::grpc::StatusCode::ABORTED);
}

TEST(FileOpsTest, WriteFileRejectsPathsOutsideRoot) {
  TempDir tmp("outside");
  zurg::file_ops::Options opts{tmp.path.string()};

  std::vector<ops::v1::FileChunk> chunks(1);
  chunks[0].set_offset(0);
  chunks[0].set_data("payload");

  ops::v1::Checksum checksum;
  checksum.set_type(ops::v1::Checksum::SHA256);
  checksum.set_digest(Sha256("payload"));

  ::grpc::Status s = zurg::file_ops::WriteFile(opts, "../evil.bin", chunks, checksum, 0644);
  EXPECT_EQ(s.error_code(), ::grpc::StatusCode::PERMISSION_DENIED);
}

TEST(FileOpsTest, WriteFileFailsWhenParentIsFile) {
  TempDir tmp("locked");
  zurg::file_ops::Options opts{tmp.path.string()};

  std::ofstream(tmp.path / "existing") << "file";

  std::vector<ops::v1::FileChunk> chunks(1);
  chunks[0].set_offset(0);
  chunks[0].set_data("payload");

  ops::v1::Checksum checksum;
  checksum.set_type(ops::v1::Checksum::SHA256);
  checksum.set_digest(Sha256("payload"));

  ::grpc::Status s = zurg::file_ops::WriteFile(opts, "existing/child.bin", chunks, checksum, 0644);
  EXPECT_EQ(s.error_code(), ::grpc::StatusCode::PERMISSION_DENIED);
}

TEST(FileOpsTest, WriteFileSuccessCreatesFile) {
  TempDir tmp("success");
  zurg::file_ops::Options opts{tmp.path.string()};

  const std::string contents = "valid payload";
  std::vector<ops::v1::FileChunk> chunks(1);
  chunks[0].set_offset(0);
  chunks[0].set_data(contents);

  ops::v1::Checksum checksum;
  checksum.set_type(ops::v1::Checksum::SHA256);
  checksum.set_digest(Sha256(contents));

  ::grpc::Status s = zurg::file_ops::WriteFile(opts, "created.bin", chunks, checksum, 0600);
  ASSERT_TRUE(s.ok());
  std::ifstream in(tmp.path / "created.bin");
  std::string loaded((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_EQ(loaded, contents);
}

TEST(FileOpsTest, RemoveFileSuccess) {
  TempDir tmp("remove_success");
  std::ofstream(tmp.path / "gone.txt") << "data";
  zurg::file_ops::Options opts{tmp.path.string()};
  ::grpc::Status s = zurg::file_ops::RemoveFile(opts, "gone.txt");
  EXPECT_TRUE(s.ok());
  EXPECT_FALSE(fs::exists(tmp.path / "gone.txt"));
}

TEST(FileOpsTest, StreamFileCancelledDuringLoop) {
  TempDir tmp("stream_cancel_loop");
  std::string data(70 * 1024, 'x');
  std::ofstream(tmp.path / "big.bin") << data;

  zurg::file_ops::Options opts{tmp.path.string()};
  ops::v1::FileGetSpec spec;
  spec.set_path("big.bin");

  int call = 0;
  auto should_stop = [&]() {
    return ++call >= 2;  // cancel on second loop iteration
  };

  int chunks = 0;
  auto consumer = [&](ops::v1::FileChunk chunk) -> ::grpc::Status {
    ++chunks;
    return ::grpc::Status::OK;
  };
  ops::v1::FileGetEof eof;
  ::grpc::Status s = zurg::file_ops::StreamFile(opts, spec, should_stop, consumer, &eof);
  EXPECT_EQ(s.error_code(), ::grpc::StatusCode::CANCELLED);
  EXPECT_GE(chunks, 1);
}

TEST(FileOpsTest, StreamFileCancelledAfterLoop) {
  TempDir tmp("stream_cancel_after");
  std::ofstream(tmp.path / "one.bin") << "1";

  zurg::file_ops::Options opts{tmp.path.string()};
  ops::v1::FileGetSpec spec;
  spec.set_path("one.bin");

  int call = 0;
  auto should_stop = [&]() {
    return ++call >= 2;  // allow first iteration, cancel after loop
  };

  auto consumer = [&](ops::v1::FileChunk) -> ::grpc::Status {
    return ::grpc::Status::OK;
  };
  ops::v1::FileGetEof eof;
  ::grpc::Status s = zurg::file_ops::StreamFile(opts, spec, should_stop, consumer, &eof);
  EXPECT_EQ(s.error_code(), ::grpc::StatusCode::CANCELLED);
  EXPECT_EQ(call, 2);
}

TEST(FileOpsTest, RemoveFileRejectsDirectory) {
  TempDir tmp("remove_dir");
  fs::create_directories(tmp.path / "nested");
  std::ofstream(tmp.path / "nested" / "file.txt") << "data";

  zurg::file_ops::Options opts{tmp.path.string()};
  ::grpc::Status s = zurg::file_ops::RemoveFile(opts, "nested");
  EXPECT_EQ(s.error_code(), ::grpc::StatusCode::FAILED_PRECONDITION);
}

TEST(FileOpsTest, RemoveFileOutsideRootDenied) {
  TempDir tmp("remove_outside");
  zurg::file_ops::Options opts{tmp.path.string()};
  ::grpc::Status s = zurg::file_ops::RemoveFile(opts, "../oops");
  EXPECT_EQ(s.error_code(), ::grpc::StatusCode::PERMISSION_DENIED);
}

TEST(FileOpsTest, RemoveFileNotFound) {
  TempDir tmp("remove_missing");
  zurg::file_ops::Options opts{tmp.path.string()};
  ::grpc::Status s = zurg::file_ops::RemoveFile(opts, "missing.txt");
  EXPECT_EQ(s.error_code(), ::grpc::StatusCode::NOT_FOUND);
}

}  // namespace
