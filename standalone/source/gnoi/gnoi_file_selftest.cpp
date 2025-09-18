#ifndef GRPC_CALLBACK_API_NONEXPERIMENTAL
#define GRPC_CALLBACK_API_NONEXPERIMENTAL 1
#endif

#include <grpcpp/grpcpp.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "github.com/openconfig/gnoi/file/file.grpc.pb.h"
#include "zurg/gnoi/file_service.h"

#if __has_include(<openssl/evp.h>)
#include <openssl/evp.h>
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
#else
static std::string sha256(const std::string&) { return {}; }
#endif

int main() {
  namespace fs = std::filesystem;
  try {
    fs::path tmp = fs::temp_directory_path() / fs::path("zurg_gnoi_selftest");
    fs::create_directories(tmp);

    grpc::ServerBuilder builder;
    int selected_port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &selected_port);
    auto svc = zurg::gnoi_file::CreateFileService(tmp.string());
    builder.RegisterService(svc.get());
    auto server = builder.BuildAndStart();
    if (!server || selected_port == 0) {
      std::cerr << "Failed to start server or select port" << std::endl;
      return 2;
    }

    auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(selected_port), grpc::InsecureChannelCredentials());
    auto stub = gnoi::file::File::NewStub(channel);

    // Put
    {
      grpc::ClientContext ctx;
      gnoi::file::PutResponse put_resp;
      auto stream = stub->Put(&ctx, &put_resp);
      if (!stream) { std::cerr << "Put stream create failed" << std::endl; return 3; }
      gnoi::file::PutRequest req;
      req.mutable_open()->set_remote_file("hello.txt");
      req.mutable_open()->set_permissions(0644);
      if (!stream->Write(req)) { std::cerr << "Put open write failed" << std::endl; return 3; }
      std::string contents = std::string(100000, 'A');
      req.Clear(); req.set_contents(contents);
      if (!stream->Write(req)) { std::cerr << "Put contents write failed" << std::endl; return 3; }
      req.Clear(); auto* h = req.mutable_hash(); h->set_method(gnoi::types::HashType::SHA256); h->set_hash(sha256(contents));
      stream->WriteLast(req, grpc::WriteOptions());
      auto s = stream->Finish(); if (!s.ok()) { std::cerr << "Put finish: " << s.error_message() << std::endl; return 3; }
    }

    // Get
    {
      grpc::ClientContext ctx;
      gnoi::file::GetRequest req; req.set_remote_file("hello.txt");
      auto stream = stub->Get(&ctx, req);
      if (!stream) { std::cerr << "Get stream create failed" << std::endl; return 4; }
      gnoi::file::GetResponse resp; std::string got;
      while (stream->Read(&resp)) {
        if (resp.has_contents()) got.append(resp.contents().data(), resp.contents().size());
      }
      auto s = stream->Finish(); if (!s.ok()) { std::cerr << "Get finish: " << s.error_message() << std::endl; return 4; }
      if (got.size() != 100000) { std::cerr << "Get size mismatch: " << got.size() << std::endl; return 4; }
    }

    // Stat
    {
      grpc::ClientContext ctx;
      gnoi::file::StatRequest req; req.set_path(".");
      gnoi::file::StatResponse resp; auto s = stub->Stat(&ctx, req, &resp);
      if (!s.ok()) { std::cerr << "Stat: " << s.error_message() << std::endl; return 5; }
      bool found = false; for (const auto& si : resp.stats()) if (fs::path(si.path()).filename() == fs::path("hello.txt")) found = true;
      if (!found) { std::cerr << "Stat did not list hello.txt" << std::endl; return 5; }
    }

    // Remove
    {
      grpc::ClientContext ctx;
      gnoi::file::RemoveRequest req; req.set_remote_file("hello.txt");
      gnoi::file::RemoveResponse resp; auto s = stub->Remove(&ctx, req, &resp);
      if (!s.ok()) { std::cerr << "Remove: " << s.error_message() << std::endl; return 6; }
    }

    server->Shutdown();
    std::cout << "GnoiFileSelfTest PASS" << std::endl;
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Exception: " << e.what() << std::endl;
    return 1;
  }
}
