#pragma once

namespace zurg::agent {

struct FeatureToggles {
  bool enabled = true;
  bool enable_log_filter = true;
  bool enable_pcap = true;
  bool enable_exec = true;
};

}  // namespace zurg::agent
