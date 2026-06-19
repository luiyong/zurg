#include "agent/system/process_info.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <thread>

#include <unistd.h>

namespace zurg::agent::system {

AgentProcessInfo GetAgentProcessInfo() {
  AgentProcessInfo info;
  info.pid = static_cast<int>(getpid());
  info.hardware_concurrency = std::thread::hardware_concurrency();
  return info;
}

std::vector<ProcessEntry> ListProcesses() {
  std::vector<ProcessEntry> out;
  FILE* pipe = popen("ps -axo pid=,ppid=,comm=", "r");
  if (!pipe) {
    return out;
  }
  char buffer[4096];
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    std::istringstream iss(buffer);
    ProcessEntry entry;
    if (!(iss >> entry.pid >> entry.parent_pid)) {
      continue;
    }
    std::getline(iss, entry.command);
    if (!entry.command.empty() && entry.command.front() == ' ') {
      entry.command.erase(0, 1);
    }
    out.push_back(std::move(entry));
  }
  pclose(pipe);
  return out;
}

nlohmann::json AgentProcessInfoToJson(const AgentProcessInfo& info) {
  return nlohmann::json{{"pid", info.pid},
                        {"hardware_concurrency", info.hardware_concurrency}};
}

nlohmann::json ProcessListToJson(const std::vector<ProcessEntry>& processes) {
  nlohmann::json items = nlohmann::json::array();
  for (const auto& process : processes) {
    items.push_back({{"pid", process.pid},
                     {"parent_pid", process.parent_pid},
                     {"command", process.command}});
  }
  return items;
}

}  // namespace zurg::agent::system
