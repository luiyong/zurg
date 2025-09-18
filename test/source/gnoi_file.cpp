#ifdef ZURG_GNOI_TESTS_ENABLED

#ifndef GRPC_CALLBACK_API_NONEXPERIMENTAL
#define GRPC_CALLBACK_API_NONEXPERIMENTAL 1
#endif

#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>
#include <chrono>
#include <fstream>
#include <string>
#include <vector>
#include <filesystem>

#include "github.com/openconfig/gnoi/file/file.grpc.pb.h"
#include "zurg/gnoi/file_service.h"

#if ZURG_HAVE_OPENSSL
#include <openssl/evp.h>
#endif

namespace fs = std::filesystem;

static std::string sha256(const std::string& data) {
#if ZURG_HAVE_OPENSSL
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (!ctx) return {};
  if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) return {};
  if (EVP_DigestUpdate(ctx, data.data(), data.size()) != 1) return {};
  unsigned int out_len = EVP_MD_size(EVP_sha256());
  std::string out(out_len, '\0');
  if (EVP_DigestFinal_ex(ctx, reinterpret_cast<unsigned char*>(&out[0]), &out_len) != 1) return {};
  EVP_MD_CTX_free(ctx);
  return out;
#else
  return std::string();
#endif
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
    auto stream = stub->Put(&ctx);
    ASSERT_TRUE(stream.get() != nullptr);
    gnoi::file::PutRequest req;
    req.mutable_open()->set_remote_file("hello.txt");
    req.mutable_open()->set_permissions(0644);
    ASSERT_TRUE(stream->Write(req));

    std::string contents = std::string(100000, 'A');
    req.Clear();
    req.set_contents(contents);
    ASSERT_TRUE(stream->Write(req));

    req.Clear();
    auto* h = req.mutable_hash();
    h->set_method(gnoi::types::HashType::SHA256);
    h->set_hash(sha256(contents));
    ASSERT_TRUE(stream->WriteLast(req, grpc::WriteOptions()));

    gnoi::file::PutResponse resp;
    grpc::Status s = stream->Finish();
    ASSERT_TRUE(s.ok()) << s.error_message();
  }

  // Get
  {
    grpc::ClientContext ctx;
    gnoi::file::GetRequest req;
    req.set_remote_file("hello.txt");
    auto stream = stub->Get(&ctx, req);
    ASSERT_TRUE(stream.get() != nullptr);
    gnoi::file::GetResponse resp;
    std::string got;
    while (stream->Read(&resp)) {
      if (resp.has_contents()) {
        got.append(resp.contents().data(), resp.contents().size());
      } else if (resp.has_hash()) {
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
    gnoi::file::RemoveRequest req;
    req.set_remote_file("hello.txt");
    gnoi::file::RemoveResponse resp;
    auto s = stub->Remove(&ctx, req, &resp);
    ASSERT_TRUE(s.ok()) << s.error_message();
  }

  server->Shutdown();
}

#endif // ZURG_GNOI_TESTS_ENABLED
