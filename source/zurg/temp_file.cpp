#include "zurg/temp_file.h"

#include <cstdio>
#include <fstream>
#include <random>
#include <stdexcept>
#include <system_error>

namespace zurg::temp_file {
namespace {

std::string GenerateSuffix() {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  std::uniform_int_distribution<std::uint64_t> dist;
  std::uint64_t value = dist(rng);
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<long long>(value));
  return std::string(buf);
}

}  // namespace

std::filesystem::path ResolveDirectory(const std::string& preferred_dir) {
  std::filesystem::path dir = preferred_dir.empty() ? std::filesystem::temp_directory_path()
                                                    : std::filesystem::path(preferred_dir);
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    throw std::filesystem::filesystem_error("failed to create temp directory", dir, ec);
  }
  return dir;
}

std::filesystem::path CreateUniquePath(const std::filesystem::path& directory,
                                       std::string_view prefix,
                                       std::string_view suffix) {
  for (int attempt = 0; attempt < 1000; ++attempt) {
    auto candidate = directory / (std::string(prefix) + GenerateSuffix() + std::string(suffix));
    if (!std::filesystem::exists(candidate)) {
      return candidate;
    }
  }
  throw std::runtime_error("failed to generate unique temp file path");
}

::grpc::Status StreamFile(const std::filesystem::path& path,
                          std::size_t chunk_size,
                          const std::function<bool()>& should_stop,
                          const std::function<::grpc::Status(std::int64_t, std::string_view)>& on_chunk) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return ::grpc::Status(::grpc::StatusCode::INTERNAL, "failed to reopen temp file");
  }
  if (chunk_size == 0) {
    chunk_size = 64 * 1024;
  }
  std::string buffer(static_cast<std::size_t>(chunk_size), '\0');
  std::int64_t offset = 0;
  while (in) {
    if (should_stop && should_stop()) {
      return ::grpc::Status(::grpc::StatusCode::CANCELLED, "operation cancelled");
    }
    in.read(buffer.data(), static_cast<std::streamsize>(chunk_size));
    std::streamsize got = in.gcount();
    if (got <= 0) break;
    auto status = on_chunk(offset, std::string_view(buffer.data(), static_cast<std::size_t>(got)));
    if (!status.ok()) {
      return status;
    }
    offset += got;
  }
  return ::grpc::Status::OK;
}

}  // namespace zurg::temp_file
