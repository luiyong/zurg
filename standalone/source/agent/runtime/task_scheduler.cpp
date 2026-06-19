#include "runtime/task_scheduler.h"

#include <algorithm>
#include <utility>

namespace zurg::agent::runtime {

TaskScheduler::TaskScheduler(TaskFactory factory, Options options,
                             std::shared_ptr<spdlog::logger> logger)
    : factory_(std::move(factory)), options_(std::move(options)), logger_(std::move(logger)) {}

TaskScheduler::~TaskScheduler() { Stop(); }

void TaskScheduler::AddSink(TaskSink* sink) {
  std::lock_guard<std::mutex> lock(mu_);
  sinks_.push_back(sink);
}

void TaskScheduler::Start() {
  std::lock_guard<std::mutex> lock(mu_);
  if (running_) {
    return;
  }
  running_ = true;
  stop_ = false;
  worker_ = std::thread(&TaskScheduler::WorkerLoop, this);
}

void TaskScheduler::Stop() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_ = true;
    running_ = false;
    if (current_) {
      current_->RequestCancel();
    }
    for (auto& task : queue_) {
      if (task) {
        task->RequestCancel();
      }
    }
  }
  cv_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
}

SubmitResult TaskScheduler::Submit(const TaskRequest& request) {
  auto built = factory_.Build(request);
  SubmitResult result;
  result.op_id = request.op_id;
  result.kind = built.kind;
  if (!built.task) {
    result.reason = built.error.empty() ? "invalid task" : std::move(built.error);
    TaskEvent rejected;
    rejected.op_id = request.op_id;
    rejected.task_kind = built.kind;
    rejected.source = request.source;
    rejected.event_kind = TaskEventKind::kRejected;
    rejected.state = tasks::Task::State::kFailed;
    rejected.message = result.reason;
    Publish(rejected);
    return result;
  }

  std::string reject_reason;
  {
    std::lock_guard<std::mutex> lock(mu_);
    result.kind = built.task->kind();
    if (drain_) {
      reject_reason = "draining";
    } else if (tasks_.count(request.op_id) > 0) {
      reject_reason = "duplicate op_id";
    } else if (!IsAllowed(built.task->kind(), &reject_reason)) {
      if (reject_reason.empty()) {
        reject_reason = "features disabled";
      }
    } else {
      tasks_[request.op_id] = built.task;
      sources_[request.op_id] = request.source;
      queue_.push_back(built.task);
      result.accepted = true;
    }
  }

  TaskEvent event;
  event.op_id = request.op_id;
  event.task_kind = result.kind;
  event.source = request.source;
  event.accepted = result.accepted;
  event.event_kind = result.accepted ? TaskEventKind::kAccepted : TaskEventKind::kRejected;
  event.state = result.accepted ? tasks::Task::State::kPending : tasks::Task::State::kFailed;
  if (!result.accepted) {
    result.reason = std::move(reject_reason);
    event.message = result.reason;
  }
  Publish(event);
  if (result.accepted) {
    cv_.notify_all();
  }
  return result;
}

SubmitResult TaskScheduler::Submit(const ops::v1::StartOp& start, TaskInputSource source) {
  TaskRequest request;
  request.op_id = start.meta().op_id();
  request.source = source;
  if (start.has_log_filter()) {
    request.spec = start.log_filter();
  } else if (start.has_pcap()) {
    request.spec = start.pcap();
  } else if (start.has_exec()) {
    request.spec = start.exec();
  } else {
    SubmitResult result;
    result.op_id = request.op_id;
    result.reason = "unsupported operation";
    TaskEvent event;
    event.op_id = request.op_id;
    event.source = source;
    event.event_kind = TaskEventKind::kRejected;
    event.state = tasks::Task::State::kFailed;
    event.message = result.reason;
    Publish(event);
    return result;
  }
  return Submit(request);
}

bool TaskScheduler::Cancel(std::uint32_t op_id, std::string reason) {
  std::shared_ptr<tasks::Task> task;
  bool queued = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = tasks_.find(op_id);
    if (it == tasks_.end()) {
      return false;
    }
    task = it->second;
    if (current_ && current_ == task) {
      task->RequestCancel();
      return true;
    }
    auto q_it = std::find(queue_.begin(), queue_.end(), task);
    if (q_it != queue_.end()) {
      queue_.erase(q_it);
      tasks_.erase(it);
      sources_.erase(op_id);
      queued = true;
    }
  }
  if (queued && task) {
    task->RequestCancel();
    TaskEvent event;
    event.op_id = op_id;
    event.task_kind = task->kind();
    event.event_kind = TaskEventKind::kError;
    event.state = tasks::Task::State::kCancelled;
    event.code = "CANCELLED";
    event.message = std::move(reason);
    Publish(event);
  }
  return true;
}

void TaskScheduler::Shutdown(bool drain) {
  std::vector<std::shared_ptr<tasks::Task>> cancel;
  {
    std::lock_guard<std::mutex> lock(mu_);
    drain_ = drain;
    if (!drain) {
      if (current_) {
        current_->RequestCancel();
      }
      cancel.assign(queue_.begin(), queue_.end());
      queue_.clear();
      for (auto& task : cancel) {
        if (task) {
          tasks_.erase(task->op_id());
          sources_.erase(task->op_id());
        }
      }
      running_ = false;
      stop_ = true;
    }
  }
  for (auto& task : cancel) {
    if (!task) continue;
    task->RequestCancel();
    TaskEvent event;
    event.op_id = task->op_id();
    event.task_kind = task->kind();
    event.event_kind = TaskEventKind::kError;
    event.state = tasks::Task::State::kCancelled;
    event.code = "CANCELLED";
    event.message = "operation cancelled";
    Publish(event);
  }
  cv_.notify_all();
}

void TaskScheduler::SetFeatures(FeatureToggles features) {
  if (!features.enabled) {
    features.enable_log_filter = false;
    features.enable_pcap = false;
    features.enable_exec = false;
  }
  std::vector<std::uint32_t> cancel_ids;
  {
    std::lock_guard<std::mutex> lock(mu_);
    options_.features = features;
    for (auto& [op_id, task] : tasks_) {
      std::string reason;
      if (task && !IsAllowed(task->kind(), &reason)) {
        task->RequestCancel();
        cancel_ids.push_back(op_id);
      }
    }
  }
  cv_.notify_all();
}

bool TaskScheduler::IsAllowed(tasks::Task::Kind kind, std::string* reason) const {
  if (!options_.features.enabled) {
    if (reason) *reason = "features disabled";
    return false;
  }
  switch (kind) {
    case tasks::Task::Kind::kLogFilter:
      if (!options_.features.enable_log_filter) {
        if (reason) *reason = "log filter disabled";
        return false;
      }
      return true;
    case tasks::Task::Kind::kPcap:
      if (!options_.features.enable_pcap) {
        if (reason) *reason = "pcap disabled";
        return false;
      }
      return true;
    case tasks::Task::Kind::kExec:
      if (!options_.features.enable_exec) {
        if (reason) *reason = "exec disabled";
        return false;
      }
      return true;
  }
  return false;
}

void TaskScheduler::Publish(const TaskEvent& event) {
  std::vector<TaskSink*> sinks;
  {
    std::lock_guard<std::mutex> lock(mu_);
    sinks = sinks_;
  }
  for (auto* sink : sinks) {
    if (sink) {
      sink->OnTaskEvent(event);
    }
  }
}

void TaskScheduler::WorkerLoop() {
  while (true) {
    std::shared_ptr<tasks::Task> task;
    TaskInputSource source = TaskInputSource::kGrpc;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
      if (stop_ && queue_.empty()) {
        break;
      }
      if (queue_.empty()) {
        continue;
      }
      task = queue_.front();
      queue_.pop_front();
      current_ = task;
      if (task) {
        auto source_it = sources_.find(task->op_id());
        if (source_it != sources_.end()) {
          source = source_it->second;
        }
      }
    }

    if (task) {
      std::vector<TaskSink*> sinks;
      {
        std::lock_guard<std::mutex> lock(mu_);
        sinks = sinks_;
      }
      TaskContextAdapter ctx(task->op_id(), task->kind(), source, std::move(sinks),
                             [this] {
                               return !options_.should_run || options_.should_run();
                             });
      ctx.PublishState(tasks::Task::State::kRunning);
      task->Run(ctx);
    }

    {
      std::lock_guard<std::mutex> lock(mu_);
      if (task) {
        tasks_.erase(task->op_id());
        sources_.erase(task->op_id());
      }
      current_.reset();
      if (drain_ && queue_.empty()) {
        stop_ = true;
      }
    }
  }
}

}  // namespace zurg::agent::runtime
