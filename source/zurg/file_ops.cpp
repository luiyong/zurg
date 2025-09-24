#include "zurg/file_ops.h"

#include <openssl/evp.h>

#include <filesystem>
#include <fstream>
#include <system_error>
#include <cerrno>

namespace zurg::file_ops {
namespace {

namespace fs = std::filesystem;

struct Sha256 {
  Sha256() { ctx_ = EVP_MD_CTX_new(); }
  ~Sha256() {
    if (ctx_) {
      EVP_MD_CTX_free(ctx_);
    }
  }
  bool Init() {
    if (!ctx_) return false;
    return EVP_DigestInit_ex(ctx_, EVP_sha256(), nullptr) == 1;
  }
  void Update(const void* data, std::size_t len) {
    if (ctx_) {
      EVP_DigestUpdate(ctx_, data, len);
    }
  }
  std::string Finalize() {
    std::string out;
    if (!ctx_) return out;  // GCOVR_EXCL_LINE
    unsigned int len = EVP_MD_size(EVP_sha256());
    out.resize(len);
    EVP_DigestFinal_ex(ctx_, reinterpret_cast<unsigned char*>(out.data()), &len);
    out.resize(len);
    return out;
  }

 private:
  EVP_MD_CTX* ctx_ = nullptr;
};

struct Root {
  bool enabled = false;
  fs::path canonical;
};

std::optional<Root> MakeRoot(const std::string& root_dir) {
  if (root_dir.empty()) return Root{};
  std::error_code ec;
  auto canonical = fs::weakly_canonical(root_dir, ec);
  if (ec) return std::nullopt;
  Root r;
  r.enabled = true;
  r.canonical = canonical;
  return r;
}

std::optional<fs::path> ResolvePath(const Root& root, const std::string& user_path) {
  fs::path input(user_path);
  fs::path target;
  if (root.enabled) {
    target = root.canonical / input.relative_path();
  } else {
    target = input;
  }
  std::error_code ec;
  fs::path normalized = fs::weakly_canonical(target, ec);
  if (ec) {
    auto parent = target.parent_path();
    normalized = fs::weakly_canonical(parent, ec);
    if (ec) return std::nullopt;
    normalized /= target.filename();
    normalized = normalized.lexically_normal();
  }
  if (root.enabled) {
    std::error_code rec;
    auto rel = fs::relative(normalized, root.canonical, rec);
    if (rec) return std::nullopt;
    for (const auto& part : rel) {
      if (part == "..") {
        return std::nullopt;
      }
    }
  }
  return normalized;
}

// GCOVR_EXCL_START
::grpc::Status MakeErrnoStatus(const std::error_code& ec, ::grpc::StatusCode fallback, std::string msg) {
  if (!ec) return ::grpc::Status::OK;
  switch (ec.value()) {
    case EACCES:
    case EPERM: return ::grpc::Status(::grpc::StatusCode::PERMISSION_DENIED, std::move(msg));
    case ENOENT: return ::grpc::Status(::grpc::StatusCode::NOT_FOUND, std::move(msg));
    case ENOTDIR:
    case EISDIR: return ::grpc::Status(::grpc::StatusCode::FAILED_PRECONDITION, std::move(msg));
    default: return ::grpc::Status(fallback, std::move(msg));
  }
}
// GCOVR_EXCL_STOP

}  // namespace

::grpc::Status StreamFile(const Options& opts,
                          const ops::v1::FileGetSpec& spec,
                          const ShouldStopFn& should_stop,
                          const FileChunkConsumer& on_chunk,
                          ops::v1::FileGetEof* eof) {
  if (!on_chunk) {
    return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "missing chunk callback");
  }
  if (!eof) {
    return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "missing eof");
  }
  eof->Clear();

  auto root = MakeRoot(opts.root_dir);
  if (!root) return ::grpc::Status(::grpc::StatusCode::FAILED_PRECONDITION, "invalid root directory");
  auto resolved = ResolvePath(*root, spec.path());
  if (!resolved) {
    return ::grpc::Status(::grpc::StatusCode::PERMISSION_DENIED, "path outside root");
  }
  if (spec.compress()) {
    return ::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, "compression not implemented");
  }
  std::ifstream in(*resolved, std::ios::binary);
  if (!in) {
    return ::grpc::Status(::grpc::StatusCode::NOT_FOUND, "file not found");
  }

  int64_t start = spec.offset();
  int64_t length = spec.length();
  in.seekg(0, std::ios::end);
  const int64_t total_size = static_cast<int64_t>(in.tellg());
  if (start < 0 || start > total_size) {
    return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "invalid offset");
  }
  in.seekg(start, std::ios::beg);
  int64_t remain = length > 0 ? length : total_size - start;

  Sha256 sha;
  if (!sha.Init()) {
    return ::grpc::Status(::grpc::StatusCode::INTERNAL, "sha init failed");  // GCOVR_EXCL_LINE
  }

  std::vector<char> buffer(64 * 1024);
  int64_t offset = start;
  while (remain > 0) {
    if (should_stop && should_stop()) {
      return ::grpc::Status(::grpc::StatusCode::CANCELLED, "operation cancelled");
    }
    std::size_t to_read = static_cast<std::size_t>(std::min<int64_t>(remain, buffer.size()));
    in.read(buffer.data(), to_read);
    std::streamsize got = in.gcount();
    if (got <= 0) break;
    sha.Update(buffer.data(), static_cast<std::size_t>(got));
    ops::v1::FileChunk chunk;
    chunk.set_offset(offset);
    chunk.set_data(buffer.data(), static_cast<std::size_t>(got));
    offset += got;
    remain -= got;
    auto status = on_chunk(std::move(chunk));
    if (!status.ok()) {
      return status;
    }
  }

  if (should_stop && should_stop()) {
    return ::grpc::Status(::grpc::StatusCode::CANCELLED, "operation cancelled");
  }

  auto computed = sha.Finalize();
  if (!spec.expect().digest().empty()) {
    if (spec.expect().type() != ops::v1::Checksum::SHA256 || spec.expect().digest() != computed) {
      return ::grpc::Status(::grpc::StatusCode::ABORTED, "checksum mismatch");
    }
    eof->mutable_checksum()->CopyFrom(spec.expect());
  } else {
    auto* checksum = eof->mutable_checksum();
    checksum->set_type(ops::v1::Checksum::SHA256);
    checksum->set_digest(computed);
  }
  eof->set_total_size(total_size);
  return ::grpc::Status::OK;
}

::grpc::Status ReadFile(const Options& opts,
                        const ops::v1::FileGetSpec& spec,
                        FileGetResult* result) {
  if (!result) return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "missing result");
  result->chunks.clear();
  auto consumer = [&](ops::v1::FileChunk chunk) -> ::grpc::Status {
    result->chunks.push_back(std::move(chunk));
    return ::grpc::Status::OK;
  };
  return StreamFile(opts, spec, ShouldStopFn{}, consumer, &result->eof);
}

::grpc::Status WriteFile(const Options& opts,
                         const std::string& remote_path,
                         const std::vector<ops::v1::FileChunk>& chunks,
                         const ops::v1::Checksum& checksum,
                         uint32_t permissions) {
  auto root = MakeRoot(opts.root_dir);
  if (!root) return ::grpc::Status(::grpc::StatusCode::FAILED_PRECONDITION, "invalid root directory");
  auto resolved = ResolvePath(*root, remote_path);
  if (!resolved) {
    return ::grpc::Status(::grpc::StatusCode::PERMISSION_DENIED, "path outside root");
  }
  fs::path target = *resolved;
  std::error_code ec;
  fs::create_directories(target.parent_path(), ec);
  fs::path tmp = target;
  tmp += ".partial";

  std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
  if (!out) {
    return ::grpc::Status(::grpc::StatusCode::PERMISSION_DENIED, "cannot open temp file");
  }
  Sha256 sha;
  if (!sha.Init()) {
    return ::grpc::Status(::grpc::StatusCode::INTERNAL, "sha init failed");  // GCOVR_EXCL_LINE
  }
  for (const auto& chunk : chunks) {
    const auto& data = chunk.data();
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    // GCOVR_EXCL_START
    if (!out) {
      out.close();
      fs::remove(tmp, ec);
      return ::grpc::Status(::grpc::StatusCode::DATA_LOSS, "write failed");
    }
    // GCOVR_EXCL_STOP
    sha.Update(data.data(), data.size());
  }
  out.flush();
  out.close();

  auto digest = sha.Finalize();
  if (checksum.type() != ops::v1::Checksum::SHA256 || checksum.digest() != digest) {
    fs::remove(tmp, ec);
    return ::grpc::Status(::grpc::StatusCode::ABORTED, "checksum mismatch");
  }

  fs::rename(tmp, target, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return MakeErrnoStatus(ec, ::grpc::StatusCode::PERMISSION_DENIED, ec.message());
  }
  fs::permissions(target, static_cast<fs::perms>(permissions), ec);
  return ::grpc::Status::OK;
}

::grpc::Status RemoveFile(const Options& opts, const std::string& remote_path) {
  auto root = MakeRoot(opts.root_dir);
  if (!root) return ::grpc::Status(::grpc::StatusCode::FAILED_PRECONDITION, "invalid root directory");
  auto resolved = ResolvePath(*root, remote_path);
  if (!resolved) {
    return ::grpc::Status(::grpc::StatusCode::PERMISSION_DENIED, "path outside root");
  }
  std::error_code ec;
  if (!fs::exists(*resolved, ec)) {
    return ::grpc::Status(::grpc::StatusCode::NOT_FOUND, "not found");
  }
  if (fs::is_directory(*resolved, ec)) {
    return ::grpc::Status(::grpc::StatusCode::FAILED_PRECONDITION, "is directory");
  }
  fs::remove(*resolved, ec);
  return MakeErrnoStatus(ec, ::grpc::StatusCode::UNKNOWN, ec.message());
}

}  // namespace zurg::file_ops
