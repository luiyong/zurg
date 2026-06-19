#pragma once

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <spdlog/logger.h>

#include "runtime/feature_toggles.h"
#include "runtime/task_context_adapter.h"
#include "runtime/task_factory.h"

namespace zurg::agent::runtime {

struct SubmitResult {
  bool accepted = false;
  std::uint32_t op_id = 0;
  tasks::Task::Kind kind = tasks::Task::Kind::kLogFilter;
  std::string reason;
};

class TaskScheduler {
 public:
  struct Options {
    FeatureToggles features;
    std::function<bool()> should_run;
  };

  TaskScheduler(TaskFactory factory, Options options,
                std::shared_ptr<spdlog::logger> logger = nullptr);
  ~TaskScheduler();

  TaskScheduler(const TaskScheduler&) = delete;
  TaskScheduler& operator=(const TaskScheduler&) = delete;

  void AddSink(TaskSink* sink);
  void Start();
  void Stop();

  SubmitResult Submit(const TaskRequest& request);
  SubmitResult Submit(const ops::v1::StartOp& start, TaskInputSource source);
  bool Cancel(std::uint32_t op_id, std::string reason = "operation cancelled");
  void Shutdown(bool drain);
  void SetFeatures(FeatureToggles features);

 private:
  bool IsAllowed(tasks::Task::Kind kind, std::string* reason) const;
  void Publish(const TaskEvent& event);
  void WorkerLoop();

  TaskFactory factory_;
  Options options_;
  std::shared_ptr<spdlog::logger> logger_;

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::vector<TaskSink*> sinks_;
  std::deque<std::shared_ptr<tasks::Task>> queue_;
  std::unordered_map<std::uint32_t, std::shared_ptr<tasks::Task>> tasks_;
  std::unordered_map<std::uint32_t, TaskInputSource> sources_;
  std::shared_ptr<tasks::Task> current_;
  bool running_ = false;
  bool stop_ = false;
  bool drain_ = false;
  std::thread worker_;
};

}  // namespace zurg::agent::runtime
