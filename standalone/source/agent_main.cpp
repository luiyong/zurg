#include "agent/agent_impl.h"

#include "os.grpc.pb.h"

#include <cxxopts.hpp>
#include <fmt/core.h>

#include <chrono>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <random>
#include <string>

namespace {

std::string GenerateFallbackAgentId() {
  using namespace std::chrono;
  auto now = system_clock::now().time_since_epoch();
  return fmt::format("agent-{}", duration_cast<milliseconds>(now).count());
}

grpc::ChannelArguments MakeChannelArgs() {
  grpc::ChannelArguments args;
  args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, 5000);
  args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 10000);
  args.SetInt(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
  args.SetInt(GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA, 0);
  args.SetInt(GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS, 5000);
  return args;
}

}  // namespace

int main(int argc, char** argv) {
  cxxopts::Options options("GnoiAgent", "Zurg gNOI agent");
  std::string target;
  std::string agent_id;

  options.add_options()
      ("t,target", "gRPC server address", cxxopts::value(target)->default_value("127.0.0.1:50051"))
      ("a,agent_id", "agent identifier", cxxopts::value(agent_id))
      ("h,help", "show help");

  const auto result = options.parse(argc, argv);
  if (result.count("help")) {
    fmt::print("{}\n", options.help());
    return 0;
  }

  if (!result.count("agent_id") || agent_id.empty()) {
    agent_id = GenerateFallbackAgentId();
  }

  auto channel = grpc::CreateCustomChannel(target, grpc::InsecureChannelCredentials(), MakeChannelArgs());
  auto stub = ops::v1::Control::NewStub(channel);

  fmt::print("[agent] starting with id '{}' connecting to {}\n", agent_id, target);
  zurg::agent::StartAgent(stub.get(), agent_id);
  fmt::print("[agent] terminated\n");
  return 0;
}
