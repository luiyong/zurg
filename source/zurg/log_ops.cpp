#include "zurg/log_ops.h"

#include <google/protobuf/util/time_util.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <set>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>
#include <ctime>

namespace zurg::log_ops {
namespace {
namespace fs = std::filesystem;

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

std::string ToLower(std::string_view in) {
  std::string out;
  out.reserve(in.size());
  for (char ch : in) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return out;
}

struct ParsedLine {
  google::protobuf::Timestamp ts;
  std::string level_lower;
  std::string_view message;
};

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

std::string MakeSuffix() {
  static std::mt19937_64 rng{std::random_device{}()};
  std::uniform_int_distribution<std::uint64_t> dist;
  std::uint64_t value = dist(rng);
  std::array<char, 17> buf{};
  std::snprintf(buf.data(), buf.size(), "%016llx", static_cast<long long>(value));
  return std::string(buf.data());
}

fs::path EnsureTempDir(const Options& opts) {
  fs::path dir = opts.temp_dir.empty() ? fs::temp_directory_path() : fs::path(opts.temp_dir);
  std::error_code ec;
  fs::create_directories(dir, ec);
  return dir;
}

::grpc::Status MakeError(::grpc::StatusCode code, std::string msg) {
  return ::grpc::Status(code, std::move(msg));
}

}  // namespace

::grpc::Status StreamLogFilter(const Options& opts,
                               const ops::v1::LogFilterSpec& spec,
                               const ShouldStopFn& should_stop,
                               const LogChunkConsumer& on_chunk,
                               ops::v1::LogFilterEof* eof) {
  if (!eof) return MakeError(::grpc::StatusCode::INVALID_ARGUMENT, "missing eof");
  if (!on_chunk) return MakeError(::grpc::StatusCode::INVALID_ARGUMENT, "missing chunk callback");
  eof->Clear();

  if (spec.compress()) {
    return MakeError(::grpc::StatusCode::UNIMPLEMENTED, "compress not supported");
  }

  auto root = MakeRoot(opts.log_root);
  if (!root) {
    return MakeError(::grpc::StatusCode::FAILED_PRECONDITION, "invalid log root");
  }

  auto resolved_base = ResolvePath(*root, spec.base_path());
  if (!resolved_base) {
    return MakeError(::grpc::StatusCode::PERMISSION_DENIED, "base path outside root");
  }

  std::vector<fs::path> candidates;
  candidates.push_back(*resolved_base);
  if (spec.include_rotations()) {
    uint32_t depth = spec.rotation_scan_depth() == 0 ? 2 : spec.rotation_scan_depth();
    auto parent = resolved_base->parent_path();
    auto stem = resolved_base->filename().string();
    auto ext = resolved_base->extension().string();
    auto stem_no_ext = resolved_base->stem().string();
    for (uint32_t i = 1; i <= depth; ++i) {
      candidates.push_back(resolved_base->string() + "." + std::to_string(i));
      if (!ext.empty()) {
        fs::path rotated = parent / (stem_no_ext + "." + std::to_string(i) + ext);
        candidates.push_back(rotated);
      }
    }
  }

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

  fs::path temp_dir = EnsureTempDir(opts);
  std::string base_name = spec.output_basename().empty() ? "logfilter" : spec.output_basename();
  fs::path temp_path = temp_dir / (base_name + "-" + MakeSuffix());

  std::ofstream out(temp_path, std::ios::binary);
  if (!out) {
    return MakeError(::grpc::StatusCode::INTERNAL, "failed to create temp file");
  }

  std::vector<std::string> used_sources;
  std::int64_t total_size = 0;
  std::int64_t total_lines = 0;

  auto enforce_limit = [&](std::size_t to_write) -> bool {
    if (spec.max_output_bytes() == 0) return true;
    return total_size + static_cast<std::int64_t>(to_write) <= static_cast<std::int64_t>(spec.max_output_bytes());
  };

  for (auto& candidate : candidates) {
    std::error_code ec;
    auto canonical = fs::weakly_canonical(candidate, ec);
    if (ec || !fs::exists(canonical)) continue;
    if (root->enabled) {
      auto rel = fs::relative(canonical, root->canonical, ec);
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

    used_sources.push_back(canonical.string());
    std::ifstream in(canonical, std::ios::binary);
    if (!in) continue;
    std::string line;
    while (std::getline(in, line)) {
      if (should_stop && should_stop()) {
        return MakeError(::grpc::StatusCode::CANCELLED, "operation cancelled");
      }
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
      total_size += static_cast<std::int64_t>(output_line.size());
      ++total_lines;
    }
  }

  out.close();

  eof->set_total_size(total_size);
  eof->set_total_lines(total_lines);
  eof->set_temp_file_path(temp_path.string());
  for (const auto& path : used_sources) {
    eof->add_source_files(path);
  }

  std::ifstream in(temp_path, std::ios::binary);
  if (!in) {
    return MakeError(::grpc::StatusCode::INTERNAL, "failed to reopen temp file");
  }
  std::vector<char> buffer(opts.chunk_size == 0 ? 64 * 1024 : opts.chunk_size);
  std::int64_t offset = 0;
  while (in) {
    if (should_stop && should_stop()) {
      return MakeError(::grpc::StatusCode::CANCELLED, "operation cancelled");
    }
    in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    std::streamsize got = in.gcount();
    if (got <= 0) break;
    ops::v1::LogChunk chunk;
    chunk.set_offset(offset);
    chunk.set_data(buffer.data(), static_cast<std::size_t>(got));
    offset += got;
    auto status = on_chunk(std::move(chunk));
    if (!status.ok()) {
      return status;
    }
  }

  in.close();

  if (opts.cleanup_temp_file) {
    std::error_code ec;
    fs::remove(temp_path, ec);
    if (ec) {
      // ignore cleanup errors
    }
  }

  return ::grpc::Status::OK;
}

}  // namespace zurg::log_ops
