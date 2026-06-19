#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace zurg::agent::system {

struct ProcessEntry {
  int pid = 0;
  int parent_pid = 0;
  std::string command;
};

struct AgentProcessInfo {
  int pid = 0;
  unsigned int hardware_concurrency = 0;
};

AgentProcessInfo GetAgentProcessInfo();
std::vector<ProcessEntry> ListProcesses();
nlohmann::json AgentProcessInfoToJson(const AgentProcessInfo& info);
nlohmann::json ProcessListToJson(const std::vector<ProcessEntry>& processes);

}  // namespace zurg::agent::system
