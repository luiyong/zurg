// Define callback API before including gRPC headers
#ifndef GRPC_CALLBACK_API_NONEXPERIMENTAL
#define GRPC_CALLBACK_API_NONEXPERIMENTAL 1
#endif

#include <grpcpp/grpcpp.h>
#include <grpcpp/server_context.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/server.h>
#include <grpcpp/security/server_credentials.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>


#include "github.com/openconfig/gnoi/file/file.grpc.pb.h"
#include "zurg/gnoi/file_service.h"

#if __has_include(<openssl/evp.h>)
#include <openssl/evp.h>
#define ZURG_HAVE_OPENSSL 1
#else
#define ZURG_HAVE_OPENSSL 0
#endif

namespace fs = std::filesystem;

namespace zurg::gnoi_file {

using grpc::CallbackServerContext;
using grpc::ServerUnaryReactor;
using grpc::ServerWriteReactor;
using grpc::ServerReadReactor;
using grpc::Status;
using grpc::StatusCode;

using gnoi::file::File;
using gnoi::file::GetRequest;
using gnoi::file::GetResponse;
using gnoi::file::PutRequest;
using gnoi::file::PutResponse;
using gnoi::file::StatRequest;
using gnoi::file::StatResponse;
using gnoi::file::StatInfo;
using gnoi::file::RemoveRequest;
using gnoi::file::RemoveResponse;
using gnoi::file::TransferToRemoteRequest;
using gnoi::file::TransferToRemoteResponse;
using gnoi::types::HashType;

namespace {

struct ShaContext {
#if ZURG_HAVE_OPENSSL
  EVP_MD_CTX* ctx = nullptr;
  const EVP_MD* md = nullptr;
#endif
  HashType::HashMethod method = HashType::SHA256;

  bool init(HashType::HashMethod m) {
    method = m;
#if ZURG_HAVE_OPENSSL
    ctx = EVP_MD_CTX_new();
    if (!ctx) return false;
    switch (method) {
      case HashType::SHA256: md = EVP_sha256(); break;
      case HashType::SHA512: md = EVP_sha512(); break;
      case HashType::MD5: md = EVP_md5(); break;
      default: return false;
    }
    return EVP_DigestInit_ex(ctx, md, nullptr) == 1;
#else
    (void)m; return true; // No-op without OpenSSL
#endif
  }
  void update(const void* data, size_t len) {
#if ZURG_HAVE_OPENSSL
    if (ctx) EVP_DigestUpdate(ctx, data, len);
#else
    (void)data; (void)len;
#endif
  }
  std::string finalize() {
#if ZURG_HAVE_OPENSSL
    std::string out;
    if (!ctx) return out;
    unsigned int out_len = EVP_MD_size(md);
    out.resize(out_len);
    EVP_DigestFinal_ex(ctx, reinterpret_cast<unsigned char*>(&out[0]), &out_len);
    EVP_MD_CTX_free(ctx);
    ctx = nullptr;
    return out;
#else
    return std::string();
#endif
  }
};

uint32_t file_mode_to_octal(const fs::perms p) {
  auto has = [&](fs::perms bit) { return (p & bit) != fs::perms::none; };
  unsigned u = (has(fs::perms::owner_read) ? 4 : 0) |
               (has(fs::perms::owner_write) ? 2 : 0) |
               (has(fs::perms::owner_exec) ? 1 : 0);
  unsigned g = (has(fs::perms::group_read) ? 4 : 0) |
               (has(fs::perms::group_write) ? 2 : 0) |
               (has(fs::perms::group_exec) ? 1 : 0);
  unsigned o = (has(fs::perms::others_read) ? 4 : 0) |
               (has(fs::perms::others_write) ? 2 : 0) |
               (has(fs::perms::others_exec) ? 1 : 0);
  return u * 100 + g * 10 + o;
}

fs::perms octal_to_file_mode(uint32_t oct) {
  unsigned u = (oct / 100) % 10;
  unsigned g = (oct / 10) % 10;
  unsigned o = (oct) % 10;
  fs::perms p = fs::perms::none;
  if (u & 4) p |= fs::perms::owner_read;
  if (u & 2) p |= fs::perms::owner_write;
  if (u & 1) p |= fs::perms::owner_exec;
  if (g & 4) p |= fs::perms::group_read;
  if (g & 2) p |= fs::perms::group_write;
  if (g & 1) p |= fs::perms::group_exec;
  if (o & 4) p |= fs::perms::others_read;
  if (o & 2) p |= fs::perms::others_write;
  if (o & 1) p |= fs::perms::others_exec;
  return p;
}

} // namespace

class FileGetReactor : public ServerWriteReactor<GetResponse> {
 public:
  FileGetReactor(std::string path, CallbackServerContext* ctx)
      : ctx_(ctx), path_(std::move(path)) {
    file_.open(path_, std::ios::in | std::ios::binary);
    if (!file_) {
      Finish(Status(grpc::StatusCode::NOT_FOUND, "file not found"));
      return;
    }
    hasher_.init(HashType::SHA256);
    StartSendChunk();
  }

  void OnWriteDone(bool ok) override {
    if (!ok) {
      Finish(Status(StatusCode::UNKNOWN, "write failed"));
      return;
    }
    if (done_) {
      Finish(Status::OK);
      return;
    }
    StartSendChunk();
  }

  void OnDone() override { delete this; }

 private:
  void StartSendChunk() {
    if (!file_) { SendHashAndFinish(); return; }
    file_.read(buffer_, sizeof(buffer_));
    std::streamsize n = file_.gcount();
    if (n <= 0) {
      file_.close();
      SendHashAndFinish();
      return;
    }
    hasher_.update(buffer_, static_cast<size_t>(n));
    msg_.Clear();
    msg_.mutable_contents()->assign(buffer_, buffer_ + n);
    StartWrite(&msg_);
  }

  void SendHashAndFinish() {
    auto digest = hasher_.finalize();
    msg_.Clear();
    auto* h = msg_.mutable_hash();
    h->set_method(HashType::SHA256);
    h->set_hash(digest);
    done_ = true;
    StartWriteAndFinish(&msg_, grpc::WriteOptions(), Status::OK);
  }

  CallbackServerContext* ctx_;
  std::ifstream file_;
  ShaContext hasher_;
  char buffer_[64 * 1024]{};
  GetResponse msg_;
  std::string path_;
  bool done_ = false;
};

class FilePutReactor : public ServerReadReactor<PutRequest> {
 public:
  explicit FilePutReactor(CallbackServerContext* ctx, std::string root)
      : ctx_(ctx), root_(std::move(root)) {
    StartRead(&req_);
  }

  void OnReadDone(bool ok) override {
    if (!ok) {
      if (!opened_) {
        Finish(Status(StatusCode::INVALID_ARGUMENT, "no open message"));
        return;
      }
      CleanupTemp();
      Finish(Status(StatusCode::ABORTED, "missing checksum; aborted"));
      return;
    }

    if (req_.has_open()) {
      if (opened_) { Finish(Status(StatusCode::INVALID_ARGUMENT, "duplicate open")); return; }
      opened_ = true;
      target_path_ = NormalizePath(req_.open().remote_file());
      permissions_ = req_.open().permissions();
      try { fs::create_directories(fs::path(target_path_).parent_path()); } catch (...) {}
      tmp_path_ = target_path_ + ".partial";
      out_.open(tmp_path_, std::ios::out | std::ios::binary | std::ios::trunc);
      if (!out_) { Finish(Status(StatusCode::PERMISSION_DENIED, "cannot open temp file")); return; }
      hasher_.init(HashType::SHA256);
      StartRead(&req_);
      return;
    }

    if (!opened_) { Finish(Status(StatusCode::INVALID_ARGUMENT, "missing open")); return; }

    if (req_.has_contents()) {
      const auto& s = req_.contents();
      out_.write(s.data(), static_cast<std::streamsize>(s.size()));
      if (!out_) { Finish(Status(StatusCode::DATA_LOSS, "write failed")); return; }
      hasher_.update(s.data(), s.size());
      StartRead(&req_);
      return;
    }

    if (req_.has_hash()) {
      out_.flush();
      out_.close();
      auto digest = hasher_.finalize();
      if (req_.hash().method() != HashType::SHA256) {
        CleanupTemp();
        Finish(Status(StatusCode::UNIMPLEMENTED, "only SHA256 supported"));
        return;
      }
      if (req_.hash().hash() != digest) {
        CleanupTemp();
        Finish(Status(StatusCode::ABORTED, "checksum mismatch"));
        return;
      }
      std::error_code ec;
      fs::rename(tmp_path_, target_path_, ec);
      if (ec) { CleanupTemp(); Finish(Status(StatusCode::PERMISSION_DENIED, std::string("rename failed: ") + ec.message())); return; }
      std::error_code ec2; fs::permissions(target_path_, octal_to_file_mode(permissions_), ec2);
      (void)ec2;
      Finish(Status::OK);
      return;
    }
  }

  void OnDone() override { delete this; }

 private:
  void CleanupTemp() {
    if (!tmp_path_.empty()) { std::error_code ec; fs::remove(tmp_path_, ec); }
  }
  std::string NormalizePath(const std::string& p) const {
    if (root_.empty()) return p;
    return (fs::weakly_canonical(fs::path(root_) / fs::path(p).relative_path())).string();
  }

  CallbackServerContext* ctx_;
  PutRequest req_;
  bool opened_ = false;
  std::string target_path_;
  std::string tmp_path_;
  std::ofstream out_;
  uint32_t permissions_ = 644;
  ShaContext hasher_;
  std::string root_;
};

class FileServiceImpl : public File::CallbackService {
 public:
  explicit FileServiceImpl(std::string root_dir = "") : root_(std::move(root_dir)) {}

  ServerWriteReactor<GetResponse>* Get(CallbackServerContext* ctx, const GetRequest* req) override {
    auto full = MakePath(req->remote_file()).string();
    return new FileGetReactor(full, ctx);
  }

  ServerReadReactor<PutRequest>* Put(CallbackServerContext* ctx, PutResponse*) override {
    return new FilePutReactor(ctx, root_);
  }

  ServerUnaryReactor* Stat(CallbackServerContext* ctx, const StatRequest* req, StatResponse* resp) override {
    auto* reactor = ctx->DefaultReactor();
    std::error_code ec;
    fs::path p = MakePath(req->path());
    if (!fs::exists(p, ec)) { reactor->Finish(Status(StatusCode::NOT_FOUND, "path not found")); return reactor; }
    auto add_stat = [&](const fs::directory_entry& de) {
      StatInfo* si = resp->add_stats();
      si->set_path(de.path().string());
      std::error_code lec;
      auto fstatus = de.status(lec);
      if (!lec) {
        auto ftime = fs::last_write_time(de, lec);
        if (!lec) {
          uint64_t ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(ftime.time_since_epoch()).count();
          si->set_last_modified(ns);
        }
        if (de.is_regular_file(lec)) si->set_size(de.file_size(lec)); else si->set_size(0);
        si->set_permissions(file_mode_to_octal(fstatus.permissions()));
        si->set_umask(0);
      }
    };
    if (fs::is_directory(p, ec)) {
      for (auto& de : fs::directory_iterator(p, ec)) { if (ec) break; add_stat(de); }
    } else {
      add_stat(fs::directory_entry(p));
    }
    reactor->Finish(Status::OK);
    return reactor;
  }

  ServerUnaryReactor* Remove(CallbackServerContext* ctx, const RemoveRequest* req, RemoveResponse*) override {
    auto* reactor = ctx->DefaultReactor();
    fs::path p = MakePath(req->remote_file());
    std::error_code ec;
    if (fs::is_directory(p, ec)) { reactor->Finish(Status(StatusCode::FAILED_PRECONDITION, "is a directory")); return reactor; }
    if (!fs::exists(p, ec)) { reactor->Finish(Status(StatusCode::NOT_FOUND, "not found")); return reactor; }
    fs::remove(p, ec);
    if (ec) reactor->Finish(Status(StatusCode::PERMISSION_DENIED, ec.message())); else reactor->Finish(Status::OK);
    return reactor;
  }

  ServerUnaryReactor* TransferToRemote(CallbackServerContext* ctx, const TransferToRemoteRequest*, TransferToRemoteResponse*) override {
    auto* reactor = ctx->DefaultReactor();
    reactor->Finish(Status(StatusCode::UNIMPLEMENTED, "TransferToRemote not implemented"));
    return reactor;
  }

 private:
  fs::path MakePath(const std::string& p) const {
    if (root_.empty()) return fs::path(p);
    return fs::weakly_canonical(fs::path(root_) / fs::path(p).relative_path());
  }
  std::string root_;
};

std::unique_ptr<File::Service> CreateFileService(const std::string& root_dir) {
  return std::make_unique<FileServiceImpl>(root_dir);
}

} // namespace zurg::gnoi_file
