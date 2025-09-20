#ifdef ZURG_GNOI_TESTS_ENABLED

#ifndef GRPC_CALLBACK_API_NONEXPERIMENTAL
#define GRPC_CALLBACK_API_NONEXPERIMENTAL 1
#endif

#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "github.com/openconfig/gnoi/file/file.grpc.pb.h"
#include "github.com/openconfig/gnoi/packet_capture/packet_capture.grpc.pb.h"
#include "zurg/gnoi/file_service.h"
#include "zurg/gnoi/packet_capture_service.h"

#if !ZURG_HAVE_OPENSSL
#error "OpenSSL support required for gNOI tests"
#endif

#include <openssl/evp.h>

namespace fs = std::filesystem;

namespace {

constexpr std::chrono::seconds kRpcTimeout{30};

inline void SetDeadline(grpc::ClientContext& ctx) {
  ctx.set_deadline(std::chrono::system_clock::now() + kRpcTimeout);
}

}  // namespace

static std::string sha256(const std::string& data) {
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (!ctx) return {};
  if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) return {};
  if (EVP_DigestUpdate(ctx, data.data(), data.size()) != 1) return {};
  unsigned int out_len = EVP_MD_size(EVP_sha256());
  std::string out(out_len, '\0');
  if (EVP_DigestFinal_ex(ctx, reinterpret_cast<unsigned char*>(&out[0]), &out_len) != 1) return {};
  EVP_MD_CTX_free(ctx);
  return out;
}

TEST(GnoiFileTest, PutGetStatRemove) {
  fs::path tmp = fs::temp_directory_path() / fs::path("zurg_gnoi_test");
  fs::create_directories(tmp);

  grpc::ServerBuilder builder;
  int selected_port = 0;
  std::string addr = "127.0.0.1:0";
  builder.AddListeningPort(addr, grpc::InsecureServerCredentials(), &selected_port);
  auto svc = zurg::gnoi_file::CreateFileService(tmp.string());
  builder.RegisterService(svc.get());
  auto server = builder.BuildAndStart();
  ASSERT_TRUE(server.get() != nullptr);
  ASSERT_NE(selected_port, 0);

  auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(selected_port), grpc::InsecureChannelCredentials());
  auto stub = gnoi::file::File::NewStub(channel);

  // Put
  {
    grpc::ClientContext ctx;
    SetDeadline(ctx);
    gnoi::file::PutResponse put_resp;
    auto stream = stub->Put(&ctx, &put_resp);
    ASSERT_TRUE(stream.get() != nullptr);
    std::cerr << "[Test] Starting Put RPC" << std::endl;
    gnoi::file::PutRequest req;
    req.mutable_open()->set_remote_file("hello.txt");
    req.mutable_open()->set_permissions(0644);
    if (!stream->Write(req)) {
      grpc::Status status = stream->Finish();
      FAIL() << "Put open write failed: code=" << status.error_code()
             << " msg=" << status.error_message();
    }

    std::string contents = std::string(100000, 'A');
    req.Clear();
    req.set_contents(contents);
    if (!stream->Write(req)) {
      grpc::Status status = stream->Finish();
      FAIL() << "Put content write failed: code=" << status.error_code()
             << " msg=" << status.error_message();
    }

    req.Clear();
    auto* h = req.mutable_hash();
    h->set_method(gnoi::types::HashType::SHA256);
    h->set_hash(sha256(contents));
    stream->WriteLast(req, grpc::WriteOptions());

    gnoi::file::PutResponse resp;
    grpc::Status s = stream->Finish();
    ASSERT_TRUE(s.ok()) << s.error_message();
  }

  // Get
  {
    grpc::ClientContext ctx;
    SetDeadline(ctx);
    gnoi::file::GetRequest req;
    req.set_remote_file("hello.txt");
    auto stream = stub->Get(&ctx, req);
    ASSERT_TRUE(stream.get() != nullptr);
    gnoi::file::GetResponse resp;
    std::string got;
    while (stream->Read(&resp)) {
      if (resp.response_case() == gnoi::file::GetResponse::kContents) {
        got.append(resp.contents().data(), resp.contents().size());
      } else if (resp.response_case() == gnoi::file::GetResponse::kHash) {
        EXPECT_EQ(resp.hash().method(), gnoi::types::HashType::SHA256);
        EXPECT_EQ(resp.hash().hash(), sha256(got));
      }
    }
    grpc::Status s = stream->Finish();
    ASSERT_TRUE(s.ok()) << s.error_message();
    EXPECT_EQ(got.size(), 100000u);
  }

  // Stat
  {
    grpc::ClientContext ctx;
    SetDeadline(ctx);
    gnoi::file::StatRequest req;
    req.set_path(".");
    gnoi::file::StatResponse resp;
    auto s = stub->Stat(&ctx, req, &resp);
    ASSERT_TRUE(s.ok()) << s.error_message();
    bool found = false;
    for (const auto& si : resp.stats()) {
      if (fs::path(si.path()).filename() == fs::path("hello.txt")) { found = true; break; }
    }
    EXPECT_TRUE(found);
  }

  // Remove
  {
    grpc::ClientContext ctx;
    SetDeadline(ctx);
    gnoi::file::RemoveRequest req;
    req.set_remote_file("hello.txt");
    gnoi::file::RemoveResponse resp;
    auto s = stub->Remove(&ctx, req, &resp);
    ASSERT_TRUE(s.ok()) << s.error_message();
  }

  server->Shutdown();
}

TEST(GnoiFileTest, RejectsPathsOutsideRoot) {
  fs::path tmp = fs::temp_directory_path() / fs::path("zurg_gnoi_root_guard");
  fs::path root = tmp / "root";
  fs::create_directories(root);
  fs::path outside = tmp / "evil.txt";
  std::ofstream(outside).put('x');

  grpc::ServerBuilder builder;
  int selected_port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &selected_port);
  auto svc = zurg::gnoi_file::CreateFileService(root.string());
  builder.RegisterService(svc.get());
  auto server = builder.BuildAndStart();
  ASSERT_TRUE(server);

  auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(selected_port), grpc::InsecureChannelCredentials());
  auto stub = gnoi::file::File::NewStub(channel);

  grpc::ClientContext ctx;
  SetDeadline(ctx);
  gnoi::file::PutResponse put_resp;
  auto stream = stub->Put(&ctx, &put_resp);
  ASSERT_TRUE(stream);

  gnoi::file::PutRequest req;
  req.mutable_open()->set_remote_file("../evil.txt");
  req.mutable_open()->set_permissions(0644);
  if (!stream->Write(req)) {
    grpc::Status status = stream->Finish();
    FAIL() << "Put open write failed: code=" << status.error_code()
           << " msg=" << status.error_message();
  }
  stream->WritesDone();
  grpc::Status status = stream->Finish();
  EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);

  server->Shutdown();
}

TEST(GnoiFileTest, CancelStopsGetStream) {
  fs::path tmp = fs::temp_directory_path() / fs::path("zurg_gnoi_cancel");
  fs::create_directories(tmp);
  fs::path file = tmp / "big.bin";
  std::string payload(256 * 1024, 'B');
  std::ofstream(file, std::ios::binary).write(payload.data(), static_cast<std::streamsize>(payload.size()));

  grpc::ServerBuilder builder;
  int selected_port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &selected_port);
  auto svc = zurg::gnoi_file::CreateFileService(tmp.string());
  builder.RegisterService(svc.get());
  auto server = builder.BuildAndStart();
  ASSERT_TRUE(server);

  auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(selected_port), grpc::InsecureChannelCredentials());
  auto stub = gnoi::file::File::NewStub(channel);

  grpc::ClientContext ctx;
  SetDeadline(ctx);
  gnoi::file::GetRequest req;
  req.set_remote_file("big.bin");
  auto stream = stub->Get(&ctx, req);
  ASSERT_TRUE(stream);

  gnoi::file::GetResponse resp;
  ASSERT_TRUE(stream->Read(&resp));
  EXPECT_EQ(resp.response_case(), gnoi::file::GetResponse::kContents);
  ctx.TryCancel();
  while (stream->Read(&resp)) {
    // drain until cancellation propagates
  }
  grpc::Status status = stream->Finish();
  EXPECT_EQ(status.error_code(), grpc::StatusCode::CANCELLED);

  server->Shutdown();
}

TEST(GnoiPcapTest, CancelStopsStreaming) {
  grpc::ServerBuilder builder;
  int selected_port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &selected_port);
  auto svc = zurg::gnoi_pcap::CreatePacketCaptureService();
  builder.RegisterService(svc.get());
  auto server = builder.BuildAndStart();
  ASSERT_TRUE(server);

  auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(selected_port), grpc::InsecureChannelCredentials());
  auto stub = gnoi::pcap::PacketCapture::NewStub(channel);

  grpc::ClientContext ctx;
  SetDeadline(ctx);
  gnoi::pcap::PcapRequest req;
  auto stream = stub->Pcap(&ctx, req);
  ASSERT_TRUE(stream);

  gnoi::pcap::PcapResponse resp;
  ASSERT_TRUE(stream->Read(&resp));
  EXPECT_GT(resp.packets_size(), 0);
  ctx.TryCancel();
  while (stream->Read(&resp)) {
    // drain
  }
  grpc::Status status = stream->Finish();
  EXPECT_EQ(status.error_code(), grpc::StatusCode::CANCELLED);

  server->Shutdown();
}

#endif // ZURG_GNOI_TESTS_ENABLED
