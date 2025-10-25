#pragma once

#include "zurg/log_ops.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace zurg::log_ops::internal {

struct Root {
  bool enabled = false;
  std::filesystem::path canonical;
};

std::optional<Root> MakeRoot(const std::string& root_dir);
std::optional<std::filesystem::path> ResolvePath(const Root& root, const std::string& user_path);

std::vector<std::filesystem::path> EnumerateCandidates(const std::filesystem::path& base_path,
                                                       bool include_rotations,
                                                       std::uint32_t rotation_depth);

struct FilterMetrics {
  std::int64_t total_size = 0;
  std::int64_t total_lines = 0;
  std::vector<std::string> source_files;
  std::filesystem::path temp_path;
};

::grpc::Status FilterLogsToTemp(const Root& root,
                                const Options& opts,
                                const ops::v1::LogFilterSpec& spec,
                                const std::vector<std::filesystem::path>& candidates,
                                FilterMetrics* metrics);

::grpc::Status StreamFilteredFile(const Options& opts,
                                  const LogChunkConsumer& on_chunk,
                                  FilterMetrics* metrics);

::grpc::Status MakeError(::grpc::StatusCode code, std::string message);

}  // namespace zurg::log_ops::internal
