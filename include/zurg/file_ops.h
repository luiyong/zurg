#pragma once

#include <grpcpp/grpcpp.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "os.pb.h"

namespace zurg::file_ops {

struct Options {
  std::string root_dir;
};

struct FileGetResult {
  std::vector<ops::v1::FileChunk> chunks;
  ops::v1::FileGetEof eof;
};

using ShouldStopFn = std::function<bool()>;
using FileChunkConsumer = std::function<::grpc::Status(ops::v1::FileChunk)>;

// 读取文件，遵守 FileGetSpec 中的 offset/length/expect 等字段。
::grpc::Status ReadFile(const Options& opts,
                        const ops::v1::FileGetSpec& spec,
                        FileGetResult* result);

// 流式读取文件：每块数据通过 on_chunk 回调发送，便于在外部实现取消逻辑。
::grpc::Status StreamFile(const Options& opts,
                          const ops::v1::FileGetSpec& spec,
                          const ShouldStopFn& should_stop,
                          const FileChunkConsumer& on_chunk,
                          ops::v1::FileGetEof* eof);

// 根据 FileChunk 数据和 Checksum 写入文件。
::grpc::Status WriteFile(const Options& opts,
                         const std::string& remote_path,
                         const std::vector<ops::v1::FileChunk>& chunks,
                         const ops::v1::Checksum& checksum,
                         uint32_t permissions);

::grpc::Status RemoveFile(const Options& opts, const std::string& remote_path);

}  // namespace zurg::file_ops
