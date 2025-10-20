#include "tasks/exec_task.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>

#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace zurg::agent::tasks {
namespace {

struct InterfaceInfo {
  std::string name;
  std::vector<std::string> addresses;
};

::grpc::Status CollectUpInterfaces(std::string* output) {
  struct ifaddrs* ifaddr = nullptr;
  if (getifaddrs(&ifaddr) == -1) {
    return ::grpc::Status(::grpc::StatusCode::INTERNAL, "getifaddrs failed");
  }

  std::unordered_map<std::string, InterfaceInfo> info;
  for (auto* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
    if (!ifa->ifa_name) continue;
    if (!(ifa->ifa_flags & IFF_UP)) continue;

    InterfaceInfo& entry = info[ifa->ifa_name];
    entry.name = ifa->ifa_name;

    if (!ifa->ifa_addr) continue;
    int family = ifa->ifa_addr->sa_family;
    char host[NI_MAXHOST];
    if (family == AF_INET || family == AF_INET6) {
      if (getnameinfo(ifa->ifa_addr,
                      (family == AF_INET) ? sizeof(struct sockaddr_in)
                                          : sizeof(struct sockaddr_in6),
                      host, sizeof(host), nullptr, 0, NI_NUMERICHOST) == 0) {
        std::string label = family == AF_INET ? "ipv4" : "ipv6";
        entry.addresses.emplace_back(label + "=" + host);
      }
    }
  }
  freeifaddrs(ifaddr);

  std::ostringstream oss;
  if (info.empty()) {
    oss << "no interfaces are up";
  } else {
    for (const auto& [name, entry] : info) {
      oss << "interface " << entry.name;
      if (!entry.addresses.empty()) {
        oss << " " << entry.addresses[0];
        for (std::size_t i = 1; i < entry.addresses.size(); ++i) {
          oss << "," << entry.addresses[i];
        }
      }
      oss << "\n";
    }
  }

  *output = oss.str();
  return ::grpc::Status::OK;
}

}  // namespace

ExecTask::ExecTask(const std::string& op_id,
                   const ops::v1::ExecSpec& spec,
                   std::shared_ptr<spdlog::logger> logger)
    : Task(op_id, Kind::kExec, std::move(logger)), spec_(spec) {}

void ExecTask::Run(TaskContext& ctx) {
  SetState(State::kRunning);

  if (!spec_.cmd().empty()) {
    std::string cmd = spec_.cmd();
    if (cmd != "ip" && cmd != "ip addr" && cmd != "ipaddress") {
      SetState(State::kFailed);
      ctx.SendError(op_id(), "UNIMPLEMENTED", "unsupported exec command");
      return;
    }
  }

  std::string output;
  ::grpc::Status status = CollectUpInterfaces(&output);
  if (!status.ok()) {
    SetState(State::kFailed);
    ctx.SendError(op_id(), std::to_string(static_cast<int>(status.error_code())), status.error_message());
    return;
  }

  if (CancelRequested() || !ctx.ShouldContinue()) {
    SetState(State::kCancelled);
    ctx.SendError(op_id(), "CANCELLED", "operation cancelled");
    return;
  }

  if (spec_.max_output_bytes() > 0 && output.size() > spec_.max_output_bytes()) {
    output.resize(static_cast<std::size_t>(spec_.max_output_bytes()));
  }

  if (!output.empty()) {
    ops::v1::ExecChunk chunk;
    chunk.mutable_stdout()->assign(output);
    ctx.SendExecData(op_id(), std::move(chunk));
  }

  ops::v1::ExecExit exit;
  exit.set_code(0);
  exit.set_note("interfaces collected");
  SetState(State::kCompleted);
  ctx.SendEofExec(op_id(), exit);
}

}  // namespace zurg::agent::tasks
