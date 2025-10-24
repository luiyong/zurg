#pragma once

#include <grpcpp/grpcpp.h>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace zurg::temp_file {

// Ensures the directory exists. If preferred_dir is empty, uses the system temp directory.
// Returns the resolved directory path (may throw if filesystem operations fail).
std::filesystem::path ResolveDirectory(const std::string& preferred_dir);

// Creates a unique file path within directory using prefix/suffix. The file is not opened.
std::filesystem::path CreateUniquePath(const std::filesystem::path& directory,
                                       std::string_view prefix,
                                       std::string_view suffix = "");

// Reads the file located at path in chunks and invokes on_chunk for each block. The callback
// receives the byte offset and a view of the payload. The should_stop callback is consulted before
// each read; if it returns true the function returns a CANCELLED status.
::grpc::Status StreamFile(const std::filesystem::path& path,
                          std::size_t chunk_size,
                          const std::function<bool()>& should_stop,
                          const std::function<::grpc::Status(std::int64_t, std::string_view)>& on_chunk);

}  // namespace zurg::temp_file

