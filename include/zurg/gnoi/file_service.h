#pragma once

#include <grpcpp/grpcpp.h>

#include <memory>
#include <string>

#include "github.com/openconfig/gnoi/file/file.grpc.pb.h"

namespace zurg::gnoi_file {

// Creates a gNOI File service backed by zurg file operations.
std::unique_ptr<gnoi::file::File::Service> CreateFileService(std::string root_dir);

}  // namespace zurg::gnoi_file

