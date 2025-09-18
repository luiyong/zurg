#ifndef GRPC_CALLBACK_API_NONEXPERIMENTAL
#define GRPC_CALLBACK_API_NONEXPERIMENTAL 1
#endif

#include <grpcpp/grpcpp.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/health_check_service_interface.h>

#include <cxxopts.hpp>
#include <spdlog/spdlog.h>

#include "zurg/gnoi/file_service.h"
#include "github.com/openconfig/gnoi/file/file.grpc.pb.h"

int main(int argc, char** argv) {
  cxxopts::Options options("GnoiFileServer", "gNOI File service (gRPC callback)");
  options.add_options()
    ("h,host", "Host:port to bind", cxxopts::value<std::string>()->default_value("0.0.0.0:50051"))
    ("r,root", "Root directory for file ops", cxxopts::value<std::string>()->default_value(""))
    ("help", "Show help");
  auto result = options.parse(argc, argv);
  if (result.count("help")) { fmt::print("{}\n", options.help()); return 0; }
  std::string addr = result["host"].as<std::string>();
  std::string root = result["root"].as<std::string>();

  grpc::EnableDefaultHealthCheckService(true);
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();

  grpc::ServerBuilder builder;
  builder.AddListeningPort(addr, grpc::InsecureServerCredentials());

  auto service = zurg::gnoi_file::CreateFileService(root);
  builder.RegisterService(service.get());

  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  if (!server) {
    spdlog::error("Failed to start server on {}", addr);
    return 1;
  }
  spdlog::info("gNOI File server listening on {} with root '{}'", addr, root);
  server->Wait();
  return 0;
}
