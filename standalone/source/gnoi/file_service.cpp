#include "zurg/gnoi/file_service.h"

#include "zurg/file_ops.h"

#include <filesystem>
#include <system_error>
#include <vector>

namespace zurg::gnoi_file {
namespace {

namespace fs = std::filesystem;

ops::v1::Checksum::Type ToChecksumType(gnoi::types::HashType::HashMethod method) {
  switch (method) {
    case gnoi::types::HashType::SHA256:
      return ops::v1::Checksum::SHA256;
    case gnoi::types::HashType::SHA512:
      return ops::v1::Checksum::SHA512;
    case gnoi::types::HashType::MD5:
      return ops::v1::Checksum::MD5;
    case gnoi::types::HashType::UNSPECIFIED:
    default:
      return ops::v1::Checksum::TYPE_UNSPECIFIED;
  }
}

gnoi::types::HashType::HashMethod ToHashMethod(ops::v1::Checksum::Type type) {
  switch (type) {
    case ops::v1::Checksum::SHA256:
      return gnoi::types::HashType::SHA256;
    case ops::v1::Checksum::SHA512:
      return gnoi::types::HashType::SHA512;
    case ops::v1::Checksum::MD5:
      return gnoi::types::HashType::MD5;
    case ops::v1::Checksum::TYPE_UNSPECIFIED:
    default:
      return gnoi::types::HashType::UNSPECIFIED;
  }
}

class FileServiceImpl final : public gnoi::file::File::Service {
 public:
  explicit FileServiceImpl(std::string root) : opts_{std::move(root)} {
    std::error_code ec;
    root_path_ = fs::weakly_canonical(opts_.root_dir, ec);
    if (ec || root_path_.empty()) {
      root_path_ = opts_.root_dir;
    }
  }

  ::grpc::Status Get(::grpc::ServerContext* ctx, const gnoi::file::GetRequest* request,
                     ::grpc::ServerWriter<gnoi::file::GetResponse>* writer) override {
    if (!request) {
      return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "missing request");
    }
    zurg::file_ops::FileGetResult result;
    ops::v1::FileGetSpec spec;
    spec.set_path(request->remote_file());
    spec.set_offset(0);
    spec.set_length(0);
    spec.set_compress(false);

    auto status = zurg::file_ops::ReadFile(opts_, spec, &result);
    if (!status.ok()) {
      return status;
    }

    for (const auto& chunk : result.chunks) {
      if (ctx && ctx->IsCancelled()) {
        return ::grpc::Status(::grpc::StatusCode::CANCELLED, "cancelled");
      }
      gnoi::file::GetResponse resp;
      resp.set_contents(chunk.data());
      if (!writer->Write(resp)) {
        if (ctx && ctx->IsCancelled()) {
          return ::grpc::Status(::grpc::StatusCode::CANCELLED, "cancelled");
        }
        return ::grpc::Status(::grpc::StatusCode::UNAVAILABLE, "stream closed");
      }
    }

    const auto& checksum = result.eof.checksum();
    if (checksum.type() != ops::v1::Checksum::TYPE_UNSPECIFIED) {
      gnoi::file::GetResponse resp;
      auto* hash = resp.mutable_hash();
      hash->set_method(ToHashMethod(checksum.type()));
      hash->set_hash(checksum.digest());
      writer->Write(resp);
    }
    return ::grpc::Status::OK;
  }

  ::grpc::Status Put(::grpc::ServerContext* /*ctx*/, ::grpc::ServerReader<gnoi::file::PutRequest>* reader,
                     gnoi::file::PutResponse* /*response*/) override {
    if (!reader) {
      return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "missing stream");
    }

    std::string remote_path;
    uint32_t permissions = 0644;
    std::vector<ops::v1::FileChunk> chunks;
    ops::v1::Checksum checksum;
    bool got_open = false;
    bool got_hash = false;
    int64_t offset = 0;

    gnoi::file::PutRequest req;
    while (reader->Read(&req)) {
      switch (req.request_case()) {
        case gnoi::file::PutRequest::kOpen:
          remote_path = req.open().remote_file();
          if (!ResolveInRoot(remote_path, nullptr)) {
            return ::grpc::Status(::grpc::StatusCode::PERMISSION_DENIED, "path outside root");
          }
          permissions = req.open().permissions();
          got_open = true;
          break;
        case gnoi::file::PutRequest::kContents: {
          ops::v1::FileChunk chunk;
          chunk.set_offset(offset);
          chunk.set_data(req.contents());
          offset += static_cast<int64_t>(req.contents().size());
          chunks.push_back(std::move(chunk));
          break;
        }
        case gnoi::file::PutRequest::kHash: {
          checksum.set_type(ToChecksumType(req.hash().method()));
          checksum.set_digest(req.hash().hash());
          got_hash = true;
          break;
        }
        case gnoi::file::PutRequest::REQUEST_NOT_SET:
        default:
          break;
      }
      req.Clear();
    }

    if (!got_open) {
      return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "missing open request");
    }
    if (!got_hash || checksum.type() == ops::v1::Checksum::TYPE_UNSPECIFIED) {
      return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "missing checksum");
    }

    auto status = zurg::file_ops::WriteFile(opts_, remote_path, chunks, checksum, permissions);
    if (!status.ok()) {
      return status;
    }
    return ::grpc::Status::OK;
  }

  ::grpc::Status Stat(::grpc::ServerContext* /*ctx*/, const gnoi::file::StatRequest* request,
                      gnoi::file::StatResponse* response) override {
    if (!request || !response) {
      return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "missing request");
    }
    fs::path target;
    if (!ResolveInRoot(request->path(), &target)) {
      return ::grpc::Status(::grpc::StatusCode::PERMISSION_DENIED, "path outside root");
    }
    std::error_code ec;
    if (!fs::exists(target, ec)) {
      return ::grpc::Status(::grpc::StatusCode::NOT_FOUND, "path not found");
    }

    auto add_entry = [&](const fs::path& entry_path) {
      gnoi::file::StatInfo info;
      std::error_code rel_ec;
      auto rel = fs::relative(entry_path, root_path_, rel_ec);
      if (!rel_ec) {
        info.set_path(rel.string());
      } else {
        info.set_path(entry_path.string());
      }
      auto status = fs::status(entry_path, ec);
      if (!ec) {
        auto perms = status.permissions();
        auto has_perm = [&](fs::perms p) { return (perms & p) != fs::perms::none; };
        auto owner = (has_perm(fs::perms::owner_read) ? 4 : 0) +
                     (has_perm(fs::perms::owner_write) ? 2 : 0) +
                     (has_perm(fs::perms::owner_exec) ? 1 : 0);
        auto group = (has_perm(fs::perms::group_read) ? 4 : 0) +
                     (has_perm(fs::perms::group_write) ? 2 : 0) +
                     (has_perm(fs::perms::group_exec) ? 1 : 0);
        auto other = (has_perm(fs::perms::others_read) ? 4 : 0) +
                     (has_perm(fs::perms::others_write) ? 2 : 0) +
                     (has_perm(fs::perms::others_exec) ? 1 : 0);
        info.set_permissions(static_cast<uint32_t>(owner * 100 + group * 10 + other));
      }
      info.set_last_modified(0);
      if (fs::is_regular_file(entry_path, ec)) {
        info.set_size(static_cast<uint64_t>(fs::file_size(entry_path, ec)));
      }
      *response->add_stats() = std::move(info);
    };

    if (fs::is_directory(target, ec)) {
      for (const auto& entry : fs::directory_iterator(target, ec)) {
        add_entry(entry.path());
      }
    } else {
      add_entry(target);
    }

    if (response->stats_size() == 0) {
      return ::grpc::Status(::grpc::StatusCode::NOT_FOUND, "no entries");
    }
    return ::grpc::Status::OK;
  }

  ::grpc::Status Remove(::grpc::ServerContext* /*ctx*/, const gnoi::file::RemoveRequest* request,
                        gnoi::file::RemoveResponse* /*response*/) override {
    if (!request) {
      return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "missing request");
    }
    return zurg::file_ops::RemoveFile(opts_, request->remote_file());
  }

  ::grpc::Status TransferToRemote(::grpc::ServerContext*, const gnoi::file::TransferToRemoteRequest*,
                                  gnoi::file::TransferToRemoteResponse*) override {
    return ::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, "not implemented");
  }

 private:
  bool ResolveInRoot(const std::string& request_path, fs::path* output) const {
    std::error_code ec;
    fs::path candidate = fs::path(opts_.root_dir) / fs::path(request_path).relative_path();
    candidate = fs::weakly_canonical(candidate, ec);
    if (ec) {
      return false;
    }
    auto rel = fs::relative(candidate, root_path_, ec);
    if (ec) {
      return false;
    }
    if (!rel.empty() && rel.native().rfind("..", 0) == 0) {
      return false;
    }
    if (output) {
      *output = std::move(candidate);
    }
    return true;
  }

  zurg::file_ops::Options opts_;
  fs::path root_path_;
};

}  // namespace

std::unique_ptr<gnoi::file::File::Service> CreateFileService(std::string root_dir) {
  return std::make_unique<FileServiceImpl>(std::move(root_dir));
}

}  // namespace zurg::gnoi_file

