#include "agent_impl.h"

#include "zurg/log_ops.h"
#include "tasks/exec_task.h"
#include "tasks/log_filter_task.h"
#include "tasks/pcap_task.h"
#include "tasks/task.h"
#include "zurg/logger_manager.h"
#include "zurg/pcap_ops.h"
#include "control/control_stream_client.h"

#include <spdlog/sinks/stdout_color_sinks.h>

#include <cstdio>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <system_error>
#include <mutex>
#include <optional>
#include <string_view>
#include <string>
#include <thread>
#include <unordered_map>

namespace zurg::agent {

namespace internal {

namespace {
std::atomic<bool> g_running{true};
std::mutex g_hook_mu;
std::function<std::chrono::milliseconds(std::size_t)> g_backoff_hook;
std::function<void(std::chrono::milliseconds)> g_sleep_hook;
std::function<void(const ops::v1::AgentToServer&)> g_send_hook;
std::once_flag g_logger_once;
std::shared_ptr<spdlog::logger> g_logger;
std::shared_ptr<spdlog::sinks::sink> g_logger_sink;
std::optional<zurg::log_ops::Options> g_log_options_override;
FeatureToggles g_feature_toggles;
}

bool IsRunning() { return g_running.load(); }

void RequestStop() { g_running.store(false); }

void ClearTestHooks() {
  std::lock_guard<std::mutex> lock(g_hook_mu);
  g_backoff_hook = nullptr;
  g_sleep_hook = nullptr;
  g_send_hook = nullptr;
}

void ResetForTests() {
  g_running.store(true);
  ClearTestHooks();
  {
    std::lock_guard<std::mutex> lock(g_hook_mu);
    g_log_options_override.reset();
    g_feature_toggles = FeatureToggles{};
  }
}

void SetBackoffHookForTests(std::function<std::chrono::milliseconds(std::size_t)> hook) {
  std::lock_guard<std::mutex> lock(g_hook_mu);
  g_backoff_hook = std::move(hook);
}

void SetSleepHookForTests(std::function<void(std::chrono::milliseconds)> hook) {
  std::lock_guard<std::mutex> lock(g_hook_mu);
  g_sleep_hook = std::move(hook);
}

void SetSendHookForTests(std::function<void(const ops::v1::AgentToServer&)> hook) {
  std::lock_guard<std::mutex> lock(g_hook_mu);
  g_send_hook = std::move(hook);
}

std::function<void(const ops::v1::AgentToServer&)> GetSendHook() {
  std::lock_guard<std::mutex> lock(g_hook_mu);
  return g_send_hook;
}

void SetLogOptions(zurg::log_ops::Options opts) {
  std::lock_guard<std::mutex> lock(g_hook_mu);
  g_log_options_override = std::move(opts);
}

std::optional<zurg::log_ops::Options> GetLogOptionsOverride() {
  std::lock_guard<std::mutex> lock(g_hook_mu);
  return g_log_options_override;
}

static FeatureToggles GetFeatureTogglesInternal() {
  std::lock_guard<std::mutex> lock(g_hook_mu);
  return g_feature_toggles;
}

static void SetFeatureTogglesInternal(FeatureToggles toggles) {
  if (!toggles.enabled) {
    toggles.enable_log_filter = false;
    toggles.enable_pcap = false;
    toggles.enable_exec = false;
  }
  std::lock_guard<std::mutex> lock(g_hook_mu);
  g_feature_toggles = std::move(toggles);
}

std::shared_ptr<spdlog::logger> GetLogger() {
  std::call_once(g_logger_once, [] {
    logging::LoggerManager::init({});
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    logging::LoggerManager::add_sink(console_sink);
    if (g_logger_sink) {
      logging::LoggerManager::add_sink(g_logger_sink);
    }
    g_logger = logging::LoggerManager::logger("agent.callback");
  });
  return g_logger;
}

void SetAdditionalLoggerSink(std::shared_ptr<spdlog::sinks::sink> sink) {
  std::lock_guard<std::mutex> lock(g_hook_mu);
  g_logger_sink = std::move(sink);
}

std::chrono::milliseconds ComputeBackoff(std::size_t attempt) {
  std::function<std::chrono::milliseconds(std::size_t)> hook;
  {
    std::lock_guard<std::mutex> lock(g_hook_mu);
    hook = g_backoff_hook;
  }
  if (hook) {
    return hook(attempt);
  }
  using namespace std::chrono;
  constexpr auto base = 500ms;
  constexpr auto max_delay = 30s;
  auto multiplier = static_cast<std::size_t>(1) << std::min<std::size_t>(attempt, 6);
  auto delay = base * multiplier;
  if (delay > max_delay) delay = max_delay;
  return delay;
}

void SleepWithStop(std::chrono::milliseconds delay) {
  std::function<void(std::chrono::milliseconds)> hook;
  {
    std::lock_guard<std::mutex> lock(g_hook_mu);
    hook = g_sleep_hook;
  }
  if (hook) {
    hook(delay);
    return;
  }
  constexpr std::chrono::milliseconds step{200};
  auto remaining = delay;
  while (IsRunning() && remaining.count() > 0) {
    auto chunk = remaining > step ? step : remaining;
    std::this_thread::sleep_for(chunk);
    remaining -= chunk;
  }
}

ops::v1::AgentToServer MakeHello(const std::string& agent_id) {
  ops::v1::AgentToServer msg;
  auto* hello = msg.mutable_hello();
  hello->set_agent_id(agent_id);
  hello->set_version("zurg-agent-dev");
  hello->set_platform("linux");
  auto* caps = hello->mutable_caps();
  caps->add_if_names("lo");
  caps->set_supports_shell(false);
  caps->set_supports_promisc(false);
  return msg;
}

ops::v1::AgentToServer MakePong(const std::string& agent_id, const ops::v1::Heartbeat& hb) {
  ops::v1::AgentToServer msg;
  auto* pong = msg.mutable_pong();
  pong->set_agent_id(agent_id);
  pong->set_seq(hb.seq());
  return msg;
}

ops::v1::AgentToServer MakeRejectAck(const ops::v1::StartOp& start) {
  ops::v1::AgentToServer msg;
  auto* ack = msg.mutable_ack();
  ack->set_op_id(start.meta().op_id());
  ack->set_accepted(false);
  ack->set_reason("operation handling not implemented");
  return msg;
}

}  // namespace internal

FeatureToggles GetFeatureToggles() {
  return internal::GetFeatureTogglesInternal();
}

void SetFeatureToggles(FeatureToggles toggles) {
  internal::SetFeatureTogglesInternal(std::move(toggles));
}

void HandleSignal(int signo) {
  if (signo == SIGINT || signo == SIGTERM) {
    internal::RequestStop();
  }
}

namespace {

using TaskShouldContinueFn = std::function<bool()>;

using tasks::ExecTask;
using tasks::LogFilterTask;
using tasks::PcapTask;
using tasks::Task;
using tasks::TaskContext;
using tasks::TaskPtr;

class ControlCallbackClient : public TaskContext {
 public:
  struct Options {
    zurg::log_ops::Options log_options;
    FeatureToggles features;
    TaskShouldContinueFn should_run;
    std::function<std::chrono::milliseconds(std::size_t)> backoff_fn;
    std::function<void(std::chrono::milliseconds)> sleep_fn;
    std::function<void(const ops::v1::AgentToServer&)> on_send;
  };

  ControlCallbackClient(ops::v1::Control::StubInterface* stub,
                        std::string agent_id,
                        Options options)
      : stub_(stub),
        agent_id_(std::move(agent_id)),
        options_(std::move(options)),
        logger_(internal::GetLogger()) {
    if (!options_.should_run) {
      options_.should_run = [] { return true; };
    }
    if (!options_.backoff_fn) {
      options_.backoff_fn = [](std::size_t attempt) { return internal::ComputeBackoff(attempt); };
    }
    if (!options_.sleep_fn) {
      options_.sleep_fn = [](std::chrono::milliseconds delay) { internal::SleepWithStop(delay); };
    }
    if (!options_.on_send) {
      options_.on_send = internal::GetSendHook();
    }
    if (options_.log_options.temp_dir.empty()) {
      std::error_code ec;
      auto tmp = std::filesystem::temp_directory_path(ec);
      if (!ec) {
        options_.log_options.temp_dir = tmp.string();
      }
    }
    if (options_.log_options.chunk_size == 0) {
      options_.log_options.chunk_size = 64 * 1024;
    }

    ControlStreamClient::Options stream_options;
    stream_options.should_run = [this] { return ShouldStreamContinue(); };
    stream_options.backoff_fn = options_.backoff_fn;
    stream_options.sleep_fn = options_.sleep_fn;
    stream_options.on_send = options_.on_send;
    stream_client_ =
        std::make_unique<ControlStreamClient>(stub_, std::move(stream_options), logger_);
    stream_client_->SetReadyCallback([this] { EnqueueWrite(internal::MakeHello(agent_id_)); });
    stream_client_->SetMessageCallback(
        [this](const ops::v1::ServerToAgent& msg, bool ok) { HandleStreamMessage(msg, ok); });
    stream_client_->SetStreamClosedCallback(
        [this](const grpc::Status& status) { HandleStreamClosed(status); });
  }

  ~ControlCallbackClient() { Stop(); }

  void Run() {
    logger_->info("starting callback client for agent {}", agent_id_);
    running_.store(true);
    worker_thread_ = std::thread(&ControlCallbackClient::WorkerLoop, this);

    stream_client_->Run();

    running_.store(false);
    ShutdownWorker();
    logger_->info("stopping callback client for agent {}", agent_id_);
  }

  void Stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
      running_.store(false);
    }
    if (stream_client_) {
      stream_client_->Stop();
    }
    ShutdownWorker();
  }

  void HandleStreamMessage(const ops::v1::ServerToAgent& msg, bool ok) {
    if (!ok) {
      return;
    }
    switch (msg.msg_case()) {
      case ops::v1::ServerToAgent::kPing:
        EnqueueWrite(internal::MakePong(agent_id_, msg.ping()));
        break;
      case ops::v1::ServerToAgent::kStart:
        HandleStartOp(msg.start());
        break;
      case ops::v1::ServerToAgent::kCancel:
        HandleCancel(msg.cancel().op_id());
        break;
      case ops::v1::ServerToAgent::kShutdown:
        HandleShutdown(msg.shutdown());
        break;
      default:
        break;
    }
  }

  void HandleStreamClosed(const grpc::Status&) {
    std::lock_guard<std::mutex> lock(mu_);
    CancelAllLocked("stream closed");
  }
  // TaskContext overrides
  bool ShouldContinue() const override {
    return ShouldContinueInternal();
  }

  void SendLogData(std::uint32_t op_id, ops::v1::LogChunk chunk) override {
    DoSendLogData(op_id, std::move(chunk));
  }

  void SendEofLog(std::uint32_t op_id, const ops::v1::LogFilterEof& eof) override {
    DoSendEofLog(op_id, eof);
  }

  void SendPcapData(std::uint32_t op_id, ops::v1::PcapPacket pkt) override {
    DoSendPcapData(op_id, std::move(pkt));
  }

  void SendEofPcap(std::uint32_t op_id, const ops::v1::PcapStats& stats) override {
    DoSendEofPcap(op_id, stats);
  }

  void SendError(std::uint32_t op_id, std::string code, std::string message) override {
    DoSendError(op_id, std::move(code), std::move(message));
  }

  void SendExecData(std::uint32_t op_id, ops::v1::ExecChunk chunk) override {
    DoSendExecData(op_id, std::move(chunk));
  }

 void SendEofExec(std::uint32_t op_id, const ops::v1::ExecExit& exit) override {
    DoSendEofExec(op_id, exit);
  }

 private:
  bool ShouldStreamContinue() const {
    if (!running_.load()) {
      return false;
    }
    if (options_.should_run && !options_.should_run()) {
      return false;
    }
    return true;
  }

  void ShutdownWorker() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      stop_worker_ = true;
    }
    task_cv_.notify_all();
    if (worker_thread_.joinable()) {
      worker_thread_.join();
    }
    stop_worker_ = false;
  }

  bool ShouldContinueInternal() const {
    if (!running_.load()) return false;
    if (!options_.should_run) return true;
    return options_.should_run();
  }

  void EnqueueWrite(ops::v1::AgentToServer msg) {
    stream_client_->EnqueueWrite(std::move(msg));
  }

  void SendAck(std::uint32_t op_id, bool accepted, std::string reason = std::string()) {
    ops::v1::AgentToServer msg;
    auto* ack = msg.mutable_ack();
    ack->set_op_id(op_id);
    ack->set_accepted(accepted);
    if (!reason.empty()) {
      ack->set_reason(std::move(reason));
    }
    if (logger_) {
      logger_->debug("send Ack op_id={} accepted={} reason={}", op_id, accepted, reason);
    }
    EnqueueWrite(std::move(msg));
  }

  void DoSendError(std::uint32_t op_id, std::string code, std::string message) {
    ops::v1::AgentToServer msg;
    auto* err = msg.mutable_error();
    err->set_op_id(op_id);
    err->set_code(std::move(code));
    err->set_message(std::move(message));
    if (logger_) {
      logger_->warn("send Error op_id={} code={} message={}", op_id, err->code(), err->message());
    }
    EnqueueWrite(std::move(msg));
  }

  void DoSendLogData(std::uint32_t op_id, ops::v1::LogChunk chunk) {
    ops::v1::AgentToServer msg;
    auto* data = msg.mutable_data();
    data->set_op_id(op_id);
    data->mutable_log_chunk()->Swap(&chunk);
    EnqueueWrite(std::move(msg));
  }

  void DoSendPcapData(std::uint32_t op_id, ops::v1::PcapPacket pkt) {
    ops::v1::AgentToServer msg;
    auto* data = msg.mutable_data();
    data->set_op_id(op_id);
    data->mutable_pcap_packet()->Swap(&pkt);
    EnqueueWrite(std::move(msg));
  }

  void DoSendEofLog(std::uint32_t op_id, const ops::v1::LogFilterEof& eof) {
    ops::v1::AgentToServer msg;
    auto* tail = msg.mutable_eof();
    tail->set_op_id(op_id);
    tail->mutable_log()->CopyFrom(eof);
    logger_->info("log filter op {} completed size={} lines={}", op_id, eof.total_size(), eof.total_lines());
    EnqueueWrite(std::move(msg));
  }

  void DoSendEofPcap(std::uint32_t op_id, const ops::v1::PcapStats& stats) {
    ops::v1::AgentToServer msg;
    auto* tail = msg.mutable_eof();
    tail->set_op_id(op_id);
    tail->mutable_pcap()->CopyFrom(stats);
    logger_->info("pcap op {} completed packets={} dropped={}", op_id, stats.received(), stats.dropped());
    EnqueueWrite(std::move(msg));
  }

  void DoSendExecData(std::uint32_t op_id, ops::v1::ExecChunk chunk) {
    ops::v1::AgentToServer msg;
    auto* data = msg.mutable_data();
    data->set_op_id(op_id);
    data->mutable_exec_chunk()->Swap(&chunk);
    EnqueueWrite(std::move(msg));
  }

  void DoSendEofExec(std::uint32_t op_id, const ops::v1::ExecExit& exit) {
    ops::v1::AgentToServer msg;
    auto* tail = msg.mutable_eof();
    tail->set_op_id(op_id);
    tail->mutable_exec()->CopyFrom(exit);
    logger_->info("exec op {} completed code={} note={}", op_id, exit.code(), exit.note());
    EnqueueWrite(std::move(msg));
  }

  void HandleStartOp(const ops::v1::StartOp& start) {
    const std::uint32_t op_id = start.meta().op_id();
    if (op_id == 0) {
      if (logger_) {
        logger_->warn("received StartOp with missing op_id");
      }
      SendAck(op_id, false, "missing op_id");
      return;
    }

    std::shared_ptr<Task> task;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (drain_mode_) {
        if (logger_) {
          logger_->warn("reject StartOp op_id={} reason=draining", op_id);
        }
        SendAck(op_id, false, "draining");
        return;
      }
      if (tasks_.count(op_id)) {
        if (logger_) {
          logger_->warn("reject StartOp op_id={} reason=duplicate", op_id);
        }
        SendAck(op_id, false, "duplicate op_id");
        return;
      }

      if (start.has_log_filter()) {
        if (!options_.features.enable_log_filter) {
          if (logger_) {
            logger_->warn("reject StartOp op_id={} reason=log_filter_disabled", op_id);
          }
          SendAck(op_id, false, "log filter disabled");
          return;
        }
        task = std::make_shared<LogFilterTask>(op_id, start.log_filter(), options_.log_options, logger_);
      } else if (start.has_pcap()) {
        if (!options_.features.enable_pcap) {
          if (logger_) {
            logger_->warn("reject StartOp op_id={} reason=pcap_disabled", op_id);
          }
          SendAck(op_id, false, "pcap disabled");
          return;
        }
        task = std::make_shared<PcapTask>(op_id, start.pcap(), options_.log_options, logger_);
      } else if (start.has_exec()) {
        if (!options_.features.enable_exec) {
          if (logger_) {
            logger_->warn("reject StartOp op_id={} reason=exec_disabled", op_id);
          }
          SendAck(op_id, false, "exec disabled");
          return;
        }
        task = std::make_shared<ExecTask>(op_id, start.exec(), logger_);
      } else {
        if (logger_) {
          logger_->warn("reject StartOp op_id={} reason=unsupported", op_id);
        }
        SendAck(op_id, false, "unsupported operation");
        return;
      }

    }

    std::string validation_error;
    if (!task->Validate(&validation_error)) {
      if (logger_) {
        logger_->warn("reject StartOp op_id={} reason={}", op_id,
                      validation_error.empty() ? "invalid parameters" : validation_error);
      }
      SendAck(op_id, false, validation_error.empty() ? "invalid parameters" : validation_error);
      return;
    }

    {
      std::lock_guard<std::mutex> lock(mu_);
      tasks_[op_id] = task;
      task_queue_.push_back(task);
    }

    const char* type_name = "unknown";
    switch (task->kind()) {
      case Task::Kind::kLogFilter:
        type_name = "log";
        break;
      case Task::Kind::kPcap:
        type_name = "pcap";
        break;
      case Task::Kind::kExec:
        type_name = "exec";
        break;
    }
    logger_->info("accepted StartOp type={} op_id={}", type_name, op_id);
    SendAck(op_id, true);
    task_cv_.notify_all();
  }

  void HandleCancel(std::uint32_t op_id) {
    std::shared_ptr<Task> target;
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = tasks_.find(op_id);
      if (it == tasks_.end()) {
        if (logger_) {
          logger_->debug("cancel ignored, op_id={} not found", op_id);
        }
        return;
      }
      target = it->second;
      if (!target) {
        tasks_.erase(it);
        return;
      }
      if (current_task_ && current_task_ == target) {
        target->RequestCancel();
        return;
      }
      auto q_it = std::find(task_queue_.begin(), task_queue_.end(), target);
      if (q_it != task_queue_.end()) {
        task_queue_.erase(q_it);
        tasks_.erase(it);
      } else {
        if (logger_) {
          logger_->debug("cancel op_id={} no longer queued", op_id);
        }
        return;
      }
    }
    if (target) {
      target->RequestCancel();
      DoSendError(op_id, "CANCELLED", "operation cancelled");
    }
  }

  void HandleShutdown(const ops::v1::Shutdown& shutdown) {
    const bool drain = shutdown.drain();
    std::vector<std::uint32_t> cancelled_ops;
    {
      std::lock_guard<std::mutex> lock(mu_);
      drain_mode_ = drain;
      if (logger_) {
        logger_->info("received shutdown request drain={} queue_size={}", drain,
                      task_queue_.size());
      }
      if (!drain) {
        if (current_task_) {
          current_task_->RequestCancel();
        }
        for (auto& task : task_queue_) {
          if (task) {
            task->RequestCancel();
            cancelled_ops.push_back(task->op_id());
          }
        }
        task_queue_.clear();
        for (auto it = tasks_.begin(); it != tasks_.end();) {
          auto& task = it->second;
          if (!task) {
            it = tasks_.erase(it);
            continue;
          }
          if (current_task_ && task == current_task_) {
            ++it;
            continue;
          }
          task->RequestCancel();
          cancelled_ops.push_back(task->op_id());
          it = tasks_.erase(it);
        }
      }
    }
    if (!drain) {
      for (const auto& op : cancelled_ops) {
        DoSendError(op, "CANCELLED", "operation cancelled");
      }
      if (stream_client_) {
        stream_client_->CancelStream();
      }
      running_.store(false);
      task_cv_.notify_all();
    }
  }

  void CancelAllLocked(std::string_view reason) {
    if (logger_) {
      std::string current = current_task_ ? std::to_string(current_task_->op_id()) : "none";
      logger_->warn("cancelling all tasks reason={} queue_size={} current={}", reason,
                    task_queue_.size(), current);
    }
    std::vector<std::uint32_t> cancelled_ops;
    for (auto it = tasks_.begin(); it != tasks_.end();) {
      auto& task = it->second;
      if (!task) {
        it = tasks_.erase(it);
        continue;
      }
      task->RequestCancel();
      if (current_task_ && task == current_task_) {
        ++it;
      } else {
        cancelled_ops.push_back(task->op_id());
        it = tasks_.erase(it);
      }
    }
    task_queue_.clear();
    for (const auto& op_id : cancelled_ops) {
      DoSendError(op_id, "CANCELLED", std::string(reason));
    }
  }
  void RunTask(const std::shared_ptr<Task>& task) {
    if (task) {
      task->Run(*this);
    }
  }

  void WorkerLoop() {
    while (running_.load()) {
      std::shared_ptr<Task> task;
      {
        std::unique_lock<std::mutex> lock(mu_);
        task_cv_.wait(lock, [&] {
          return stop_worker_ || !running_.load() || !task_queue_.empty();
        });
        if ((stop_worker_ || !running_.load()) && task_queue_.empty()) {
          break;
        }
        if (task_queue_.empty()) {
          continue;
        }
        task = task_queue_.front();
        task_queue_.pop_front();
        current_task_ = task;
        if (logger_ && task) {
          const char* type_name = "unknown";
          switch (task->kind()) {
            case Task::Kind::kLogFilter:
              type_name = "log";
              break;
            case Task::Kind::kPcap:
              type_name = "pcap";
              break;
            case Task::Kind::kExec:
              type_name = "exec";
              break;
          }
          logger_->info("starting task op_id={} type={}", task->op_id(), type_name);
        }
      }

      RunTask(task);

      {
        std::lock_guard<std::mutex> lock(mu_);
        if (task) {
          tasks_.erase(task->op_id());
          if (logger_) {
            const char* type_name = "unknown";
            switch (task->kind()) {
              case Task::Kind::kLogFilter:
                type_name = "log";
                break;
              case Task::Kind::kPcap:
                type_name = "pcap";
                break;
              case Task::Kind::kExec:
                type_name = "exec";
                break;
            }
            logger_->info("task finished op_id={} type={} state={}",
                          task->op_id(), type_name, static_cast<int>(task->state()));
          }
        }
        current_task_.reset();
        if (drain_mode_ && task_queue_.empty()) {
          running_.store(false);
        }
      }
    }
  }

  ops::v1::Control::StubInterface* stub_ = nullptr;
  std::string agent_id_;
  Options options_;
  std::shared_ptr<spdlog::logger> logger_;
  std::unique_ptr<ControlStreamClient> stream_client_;

  std::atomic<bool> running_{false};
  std::thread worker_thread_;

  std::mutex mu_;
  std::condition_variable task_cv_;
  std::deque<std::shared_ptr<Task>> task_queue_;
  std::unordered_map<std::uint32_t, std::shared_ptr<Task>> tasks_;
  std::shared_ptr<Task> current_task_;
  bool drain_mode_ = false;
  bool stop_worker_ = false;
};

}  // namespace

void StartAgent(ops::v1::Control::StubInterface* stub, const std::string& agent_id) {
  if (!stub) {
    auto logger = internal::GetLogger();
    if (logger) {
      logger->error("missing stub, aborting agent startup");
    } else {
      std::fprintf(stderr, "[agent] missing stub, aborting\n");
    }
    return;
  }

  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);

  ControlCallbackClient::Options options;
  options.should_run = [] { return internal::IsRunning(); };
  options.backoff_fn = [](std::size_t attempt) { return internal::ComputeBackoff(attempt); };
  options.sleep_fn = [](std::chrono::milliseconds delay) { internal::SleepWithStop(delay); };
  if (auto override_opts = internal::GetLogOptionsOverride()) {
    options.log_options = *override_opts;
  }
  options.features = GetFeatureToggles();

  ControlCallbackClient client(stub, agent_id, options);
  client.Run();
}

}  // namespace zurg::agent
