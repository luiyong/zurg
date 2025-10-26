#include "zurg/log_ops.h"

#include "zurg/log_ops_internal.h"
#include "zurg/temp_file.h"

#include <google/protobuf/util/time_util.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>
#include <ctime>

namespace zurg::log_ops {
namespace fs = std::filesystem;

namespace internal {

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
  if (!root.enabled || input.is_absolute()) {
    target = input;
  } else {
    target = root.canonical / input;
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

std::vector<fs::path> EnumerateCandidates(const fs::path& base_path,
                                          bool include_rotations,
                                          std::uint32_t rotation_depth) {
  std::vector<fs::path> candidates;
  candidates.push_back(base_path);
  if (!include_rotations) return candidates;

  std::uint32_t depth = rotation_depth == 0 ? 2u : rotation_depth;
  auto parent = base_path.parent_path();
  auto stem = base_path.filename().string();
  auto ext = base_path.extension().string();
  auto stem_no_ext = base_path.stem().string();
  for (std::uint32_t i = 1; i <= depth; ++i) {
    candidates.push_back(base_path.string() + "." + std::to_string(i));
    if (!ext.empty()) {
      fs::path rotated = parent / (stem_no_ext + "." + std::to_string(i) + ext);
      candidates.push_back(rotated);
    }
  }
  return candidates;
}

struct ParsedLine {
  google::protobuf::Timestamp ts;
  std::string level_lower;
  std::string_view message;
};

std::string ToLower(std::string_view in) {
  std::string out;
  out.reserve(in.size());
  for (char ch : in) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return out;
}

bool ParseSpdlogLine(std::string_view line, ParsedLine* parsed) {
  if (line.size() < 22 || line.front() != '[') return false;
  auto close_time = line.find(']');
  if (close_time == std::string_view::npos || close_time < 20) return false;
  std::string_view time_part = line.substr(1, close_time - 1);
  if (time_part.size() < 19) return false;
  auto read_int = [&](std::size_t pos, std::size_t len) -> std::optional<int> {
    if (pos + len > time_part.size()) return std::nullopt;
    int value = 0;
    for (std::size_t i = 0; i < len; ++i) {
      char c = time_part[pos + i];
      if (c < '0' || c > '9') return std::nullopt;
      value = value * 10 + (c - '0');
    }
    return value;
  };
  auto year = read_int(0, 4);
  auto month = read_int(5, 2);
  auto day = read_int(8, 2);
  auto hour = read_int(11, 2);
  auto minute = read_int(14, 2);
  auto second = read_int(17, 2);
  if (!year || !month || !day || !hour || !minute || !second) return false;
  int nanos = 0;
  if (time_part.size() > 19 && time_part[19] == '.') {
    std::size_t frac_len = std::min<std::size_t>(9, time_part.size() - 20);
    int factor = 100000000;
    for (std::size_t i = 0; i < frac_len; ++i) {
      char c = time_part[20 + i];
      if (c < '0' || c > '9') break;
      nanos += (c - '0') * factor;
      factor /= 10;
    }
  }

  std::tm tm = {};
  tm.tm_year = *year - 1900;
  tm.tm_mon = *month - 1;
  tm.tm_mday = *day;
  tm.tm_hour = *hour;
  tm.tm_min = *minute;
  tm.tm_sec = *second;
#if defined(_WIN32)
  std::time_t secs = _mkgmtime(&tm);
#else
  std::time_t secs = timegm(&tm);
#endif
  if (secs == static_cast<std::time_t>(-1)) return false;
  parsed->ts.set_seconds(static_cast<std::int64_t>(secs));
  parsed->ts.set_nanos(nanos);

  auto level_open = line.find('[', close_time + 1);
  if (level_open == std::string_view::npos) return false;
  auto level_close = line.find(']', level_open + 1);
  if (level_close == std::string_view::npos) return false;
  auto second_bracket = line.find('[', level_close + 1);
  if (second_bracket == std::string_view::npos) return false;
  auto second_close = line.find(']', second_bracket + 1);
  if (second_close == std::string_view::npos) return false;
  std::string_view level_part = line.substr(second_bracket + 1, second_close - second_bracket - 1);
  parsed->level_lower = ToLower(level_part);
  if (second_close + 2 <= line.size()) {
    parsed->message = line.substr(second_close + 2);
  } else {
    parsed->message = std::string_view{};
  }
  return true;
}

struct TimeRange {
  bool has_start = false;
  google::protobuf::Timestamp start;
  bool has_end = false;
  google::protobuf::Timestamp end;
};

bool TimestampLess(const google::protobuf::Timestamp& lhs, const google::protobuf::Timestamp& rhs) {
  if (lhs.seconds() < rhs.seconds()) return true;
  if (lhs.seconds() > rhs.seconds()) return false;
  return lhs.nanos() < rhs.nanos();
}

bool InRange(const ParsedLine& line, const TimeRange& range) {
  if (range.has_start && TimestampLess(line.ts, range.start)) return false;
  if (range.has_end && TimestampLess(range.end, line.ts)) return false;
  return true;
}

bool InLevelFilter(const ParsedLine& line, const std::vector<std::string>& levels) {
  if (levels.empty()) return true;
  for (const auto& allowed : levels) {
    if (line.level_lower == allowed) return true;
  }
  return false;
}

bool ContainsFilter(std::string_view haystack, const std::string& needle) {
  if (needle.empty()) return true;
  return haystack.find(needle) != std::string::npos;
}

::grpc::Status MakeError(::grpc::StatusCode code, std::string message) {
  return ::grpc::Status(code, std::move(message));
}

::grpc::Status FilterLogsToTemp(const Root& root,
                                const Options& opts,
                                const ops::v1::LogFilterSpec& spec,
                                const std::vector<fs::path>& candidates,
                                FilterMetrics* metrics) {
  if (!metrics) {
    return MakeError(::grpc::StatusCode::INVALID_ARGUMENT, "missing metrics");
  }
  metrics->total_size = 0;
  metrics->total_lines = 0;
  metrics->source_files.clear();
  metrics->temp_path.clear();

  std::vector<std::string> level_filters;
  level_filters.reserve(spec.level_in_size());
  for (const auto& lvl : spec.level_in()) {
    level_filters.push_back(ToLower(lvl));
  }

  TimeRange range;
  if (spec.has_start_time()) {
    range.has_start = true;
    range.start = spec.start_time();
  }
  if (spec.has_end_time()) {
    range.has_end = true;
    range.end = spec.end_time();
  }

  std::string contains = spec.grep_contains();

  fs::path temp_dir;
  try {
    temp_dir = temp_file::ResolveDirectory(opts.temp_dir);
  } catch (const std::filesystem::filesystem_error& err) {
    return MakeError(::grpc::StatusCode::INTERNAL, err.code().message());
  }

  std::string base_name = opts.output_basename.empty() ? "logfilter" : opts.output_basename;
  try {
    metrics->temp_path = temp_file::CreateUniquePath(temp_dir, base_name + "-", "");
  } catch (const std::exception& ex) {
    return MakeError(::grpc::StatusCode::INTERNAL, ex.what());
  }

  std::ofstream out(metrics->temp_path, std::ios::binary);
  if (!out) {
    return MakeError(::grpc::StatusCode::INTERNAL, "failed to create temp file");
  }

  auto enforce_limit = [&](std::size_t to_write) -> bool {
    if (spec.max_output_bytes() == 0) return true;
    return metrics->total_size + static_cast<std::int64_t>(to_write) <=
           static_cast<std::int64_t>(spec.max_output_bytes());
  };

  for (const auto& candidate : candidates) {
    std::error_code ec;
    auto canonical = fs::weakly_canonical(candidate, ec);
    if (ec || !fs::exists(canonical)) continue;
    if (root.enabled) {
      auto rel = fs::relative(canonical, root.canonical, ec);
      if (ec) continue;
      bool ok = true;
      for (const auto& part : rel) {
        if (part == "..") {
          ok = false;
          break;
        }
      }
      if (!ok) continue;
    }

    metrics->source_files.push_back(canonical.string());
    std::ifstream in(canonical, std::ios::binary);
    if (!in) continue;
    std::string line;
    while (std::getline(in, line)) {
      ParsedLine parsed_line;
      if (!ParseSpdlogLine(line, &parsed_line)) {
        continue;
      }
      if (!InRange(parsed_line, range)) continue;
      if (!InLevelFilter(parsed_line, level_filters)) continue;
      if (!ContainsFilter(line, contains)) continue;
      std::string output_line = line;
      output_line.push_back('\n');
      if (!enforce_limit(output_line.size())) {
        return MakeError(::grpc::StatusCode::RESOURCE_EXHAUSTED, "output exceeds limit");
      }
      out.write(output_line.data(), static_cast<std::streamsize>(output_line.size()));
      if (!out) {
        return MakeError(::grpc::StatusCode::INTERNAL, "failed to write temp file");
      }
      metrics->total_size += static_cast<std::int64_t>(output_line.size());
      ++metrics->total_lines;
    }
  }

  out.close();
  return ::grpc::Status::OK;
}

::grpc::Status StreamFilteredFile(const Options& opts,
                                  const LogChunkConsumer& on_chunk,
                                  FilterMetrics* metrics) {
  auto stream_status = temp_file::StreamFile(
      metrics->temp_path, opts.chunk_size, {},
      [&](std::int64_t offset, std::string_view data) -> ::grpc::Status {
        ops::v1::LogChunk chunk;
        chunk.set_offset(offset);
        chunk.set_data(data.data(), data.size());
        return on_chunk(std::move(chunk));
      });
  if (!stream_status.ok()) {
    return stream_status;
  }
  if (opts.cleanup_temp_file) {
    std::error_code ec;
    fs::remove(metrics->temp_path, ec);
  }
  return ::grpc::Status::OK;
}

}  // namespace internal

::grpc::Status StreamLogFilter(const Options& opts,
                               const ops::v1::LogFilterSpec& spec,
                               const LogChunkConsumer& on_chunk,
                               ops::v1::LogFilterEof* eof) {
  if (!eof) return internal::MakeError(::grpc::StatusCode::INVALID_ARGUMENT, "missing eof");
  if (!on_chunk) return internal::MakeError(::grpc::StatusCode::INVALID_ARGUMENT, "missing chunk callback");
  eof->Clear();

  if (spec.compress()) {
    return internal::MakeError(::grpc::StatusCode::UNIMPLEMENTED, "compress not supported");
  }

  auto root = internal::MakeRoot(opts.log_root);
  if (!root) {
    return internal::MakeError(::grpc::StatusCode::FAILED_PRECONDITION, "invalid log root");
  }

  if (opts.base_path.empty()) {
    return internal::MakeError(::grpc::StatusCode::FAILED_PRECONDITION, "missing log base path");
  }

  auto resolved_base = internal::ResolvePath(*root, opts.base_path);
  if (!resolved_base) {
    return internal::MakeError(::grpc::StatusCode::PERMISSION_DENIED, "base path outside root");
  }

  auto candidates = internal::EnumerateCandidates(*resolved_base, opts.include_rotations,
                                                  opts.rotation_scan_depth);

  internal::FilterMetrics metrics;
  auto filter_status = internal::FilterLogsToTemp(*root, opts, spec, candidates, &metrics);
  if (!filter_status.ok()) {
    if (!metrics.temp_path.empty() && opts.cleanup_temp_file) {
      std::error_code ec;
      fs::remove(metrics.temp_path, ec);
    }
    return filter_status;
  }

  auto stream_status = internal::StreamFilteredFile(opts, on_chunk, &metrics);
  if (!stream_status.ok()) {
    if (!metrics.temp_path.empty() && opts.cleanup_temp_file) {
      std::error_code ec;
      fs::remove(metrics.temp_path, ec);
    }
    return stream_status;
  }

  eof->set_total_size(metrics.total_size);
  eof->set_total_lines(metrics.total_lines);
  eof->set_temp_file_path(metrics.temp_path.string());
  for (const auto& path : metrics.source_files) {
    eof->add_source_files(path);
  }

  if (opts.cleanup_temp_file) {
    std::error_code ec;
    fs::remove(metrics.temp_path, ec);
  }

  return ::grpc::Status::OK;
}

}  // namespace zurg::log_ops
