#include "agent/agent_impl.h"

#include "os.grpc.pb.h"

#include <cxxopts.hpp>
#include <fmt/core.h>
#include <yaml-cpp/yaml.h>

#include <chrono>
#include <grpcpp/grpcpp.h>
#include <filesystem>
#include <memory>
#include <optional>
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

std::optional<zurg::agent::FeatureToggles> LoadFeatureTogglesFromConfig(const std::string& path,
                                                                        std::string* error) {
  try {
    YAML::Node root = YAML::LoadFile(path);
    zurg::agent::FeatureToggles toggles;
    if (auto features = root["features"]) {
      toggles.enable_log_filter = features["log_filter"].as<bool>(toggles.enable_log_filter);
      toggles.enable_pcap = features["pcap"].as<bool>(toggles.enable_pcap);
      toggles.enable_exec = features["exec"].as<bool>(toggles.enable_exec);
    }
    return toggles;
  } catch (const std::exception& ex) {
    if (error) {
      *error = ex.what();
    }
    return std::nullopt;
  }
}

}  // namespace

int main(int argc, char** argv) {
  cxxopts::Options options("GnoiAgent", "Zurg gNOI agent");
  std::string target;
  std::string agent_id;
  std::string config_path;

  options.add_options()
      ("t,target", "gRPC server address", cxxopts::value(target)->default_value("127.0.0.1:50051"))
      ("a,agent_id", "agent identifier", cxxopts::value(agent_id))
      ("c,config", "YAML config file", cxxopts::value(config_path))
      ("h,help", "show help");

  const auto result = options.parse(argc, argv);
  if (result.count("help")) {
    fmt::print("{}\n", options.help());
    return 0;
  }

  if (!result.count("agent_id") || agent_id.empty()) {
    agent_id = GenerateFallbackAgentId();
  }

  if (!config_path.empty()) {
    if (!std::filesystem::exists(config_path)) {
      fmt::print(stderr, "[agent] config file '{}' not found\n", config_path);
      return 1;
    }
    std::string error;
    auto toggles = LoadFeatureTogglesFromConfig(config_path, &error);
    if (!toggles) {
      fmt::print(stderr, "[agent] failed to load config '{}': {}\n", config_path, error);
      return 1;
    }
    zurg::agent::SetFeatureToggles(*toggles);
  }

  auto active_toggles = zurg::agent::GetFeatureToggles();

  auto channel = grpc::CreateCustomChannel(target, grpc::InsecureChannelCredentials(), MakeChannelArgs());
  auto stub = ops::v1::Control::NewStub(channel);

  fmt::print("[agent] starting with id '{}' connecting to {}\n", agent_id, target);
  fmt::print("[agent] feature toggles: log_filter={} pcap={} exec={}\n",
             active_toggles.enable_log_filter ? "on" : "off",
             active_toggles.enable_pcap ? "on" : "off",
             active_toggles.enable_exec ? "on" : "off");
  zurg::agent::StartAgent(stub.get(), agent_id);
  fmt::print("[agent] terminated\n");
  return 0;
}
