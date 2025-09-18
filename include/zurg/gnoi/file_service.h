#pragma once

#ifndef GRPC_CALLBACK_API_NONEXPERIMENTAL
#define GRPC_CALLBACK_API_NONEXPERIMENTAL 1
#endif

#include <memory>
#include <string>

#include "github.com/openconfig/gnoi/file/file.grpc.pb.h"

namespace zurg::gnoi_file {

// Create a gNOI File service instance using gRPC callback API.
// root_dir: optional base directory for file operations; empty means absolute paths as-is.
std::unique_ptr<gnoi::file::File::Service> CreateFileService(const std::string& root_dir);

} // namespace zurg::gnoi_file
