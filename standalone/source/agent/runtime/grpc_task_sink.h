#pragma once

#include <functional>

#include "os.grpc.pb.h"
#include "runtime/task_sink.h"

namespace zurg::agent::runtime {

class GrpcTaskSink : public TaskSink {
 public:
  using SendFn = std::function<void(ops::v1::AgentToServer)>;

  explicit GrpcTaskSink(SendFn send_fn);

  void OnTaskEvent(const TaskEvent& event) override;

 private:
  SendFn send_fn_;
};

}  // namespace zurg::agent::runtime
