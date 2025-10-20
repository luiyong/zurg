#pragma once

#include <grpcpp/grpcpp.h>

#include <cstddef>
#include <functional>
#include <string>

#include "os.pb.h"

namespace zurg::log_ops {

struct Options {
  std::string log_root;
  std::string temp_dir;
  std::size_t chunk_size = 64 * 1024;
  bool cleanup_temp_file = true;
};

using ShouldStopFn = std::function<bool()>;
using LogChunkConsumer = std::function<::grpc::Status(ops::v1::LogChunk)>;

::grpc::Status StreamLogFilter(const Options& opts,
                               const ops::v1::LogFilterSpec& spec,
                               const ShouldStopFn& should_stop,
                               const LogChunkConsumer& on_chunk,
                               ops::v1::LogFilterEof* eof);

}  // namespace zurg::log_ops

