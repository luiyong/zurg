#include "agent_impl.h"

#include "zurg/log_ops.h"
#include "tasks/log_filter_task.h"
#include "tasks/pcap_task.h"
#include "tasks/task.h"
#include "zurg/logger_manager.h"
#include "zurg/pcap_ops.h"

#include <spdlog/sinks/stdout_color_sinks.h>

#include <cstdio>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <system_error>
#include <mutex>
#include <optional>
#include <string_view>
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

void SetLogOptionsForTests(zurg::log_ops::Options opts) {
  std::lock_guard<std::mutex> lock(g_hook_mu);
  g_log_options_override = std::move(opts);
}

std::optional<zurg::log_ops::Options> GetLogOptionsOverride() {
  std::lock_guard<std::mutex> lock(g_hook_mu);
  return g_log_options_override;
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

void SetLoggerSinkForTests(std::shared_ptr<spdlog::sinks::sink> sink) {
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

void HandleSignal(int signo) {
  if (signo == SIGINT || signo == SIGTERM) {
    internal::RequestStop();
  }
}

namespace {

using TaskShouldContinueFn = std::function<bool()>;

class ControlCallbackClient;

class ControlStreamReactor : public grpc::ClientBidiReactor<ops::v1::AgentToServer, ops::v1::ServerToAgent> {
 public:
  ControlStreamReactor(ControlCallbackClient* parent,
                       ops::v1::Control::StubInterface* stub,
                       grpc::ClientContext* ctx);

  void Begin();
  grpc::Status Wait();
  void InjectMessage(const ops::v1::ServerToAgent& msg);
  void TryCancel() { context_->TryCancel(); }

  void OnReadDone(bool ok) override;
  void OnWriteDone(bool ok) override;
  void OnDone(const ::grpc::Status& status) override;

 private:
  ControlCallbackClient* parent_;
  grpc::ClientContext* context_;
  ops::v1::ServerToAgent incoming_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool done_ = false;
  grpc::Status status_ = grpc::Status::OK;
};

using tasks::LogFilterTask;
using tasks::PcapTask;
using tasks::Task;
using tasks::TaskContext;
using tasks::TaskPtr;

class ControlCallbackClient : public TaskContext {
 public:
  struct Options {
    zurg::log_ops::Options log_options;
    TaskShouldContinueFn should_run;
    std::function<std::chrono::milliseconds(std::size_t)> backoff_fn;
    std::function<void(std::chrono::milliseconds)> sleep_fn;
    std::function<void(const ops::v1::AgentToServer&)> on_send;
  };

  ControlCallbackClient(ops::v1::Control::StubInterface* stub,
                        std::string agent_id,
                        Options options)
      : stub_(stub), agent_id_(std::move(agent_id)), options_(std::move(options)),
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
  }

  ~ControlCallbackClient() { Stop(); }

  void Run() {
    logger_->info("starting callback client for agent {}", agent_id_);
    running_.store(true);
    worker_thread_ = std::thread(&ControlCallbackClient::WorkerLoop, this);

    std::size_t attempt = 0;
    while (options_.should_run && options_.should_run()) {
      grpc::ClientContext ctx;
      logger_->info("connecting to control stream (attempt={})", attempt + 1);
      ControlStreamReactor reactor(this, stub_, &ctx);
      reactor.Begin();
      grpc::Status status = reactor.Wait();

      bool graceful = false;
      {
        std::lock_guard<std::mutex> lock(mu_);
        graceful = drain_mode_ && task_queue_.empty() && !current_task_;
      }
      if (!options_.should_run() || graceful) {
        running_.store(false);
        break;
      }
      ++attempt;
      auto delay = options_.backoff_fn(attempt);
      logger_->warn("stream closed (code={}, message='{}'), reconnecting in {} ms",
                    static_cast<int>(status.error_code()), status.error_message(), delay.count());
      options_.sleep_fn(delay);
    }

    logger_->info("stopping callback client for agent {}", agent_id_);
    Stop();
  }

  void Stop() {
    running_.store(false);
    {
      std::lock_guard<std::mutex> lock(mu_);
      stop_worker_ = true;
    }
    task_cv_.notify_all();
    if (worker_thread_.joinable()) {
      worker_thread_.join();
    }
  }

  void OnStreamReady(ControlStreamReactor* reactor) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      reactor_ = reactor;
    }
    EnqueueWrite(internal::MakeHello(agent_id_));
  }

  void OnWriteFinished(bool ok) {
    std::lock_guard<std::mutex> lock(mu_);
    if (logger_) {
      logger_->debug("write finished ok={} queue={} in_flight_before={} pending={} current={}", ok,
                     task_queue_.size(), write_in_flight_, pending_writes_.size(),
                     current_write_ ? current_write_->msg_case() : 0);
    }
    write_in_flight_ = false;
    current_write_.reset();
    if (!ok) {
      pending_writes_.clear();
    }
    MaybeStartWriteLocked();
  }

  void OnMessage(const ops::v1::ServerToAgent& msg, bool ok) {
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

  void OnStreamClosed(const grpc::Status&) {
    std::lock_guard<std::mutex> lock(mu_);
    reactor_ = nullptr;
    write_in_flight_ = false;
    current_write_.reset();
    pending_writes_.clear();
    CancelAllLocked("stream closed");
  }
  // TaskContext overrides
  bool ShouldContinue() const override {
    return ShouldContinueInternal();
  }

  void SendLogData(const std::string& op_id, ops::v1::LogChunk chunk) override {
    DoSendLogData(op_id, std::move(chunk));
  }

  void SendEofLog(const std::string& op_id, const ops::v1::LogFilterEof& eof) override {
    DoSendEofLog(op_id, eof);
  }

  void SendPcapData(const std::string& op_id, ops::v1::PcapPacket pkt) override {
    DoSendPcapData(op_id, std::move(pkt));
  }

  void SendEofPcap(const std::string& op_id, const ops::v1::PcapStats& stats) override {
    DoSendEofPcap(op_id, stats);
  }

  void SendError(const std::string& op_id, std::string code, std::string message) override {
    DoSendError(op_id, std::move(code), std::move(message));
  }

 private:
  bool ShouldContinueInternal() const {
    if (!running_.load()) return false;
    if (!options_.should_run) return true;
    return options_.should_run();
  }

  void MaybeStartWriteLocked() {
    if (!reactor_ || write_in_flight_ || pending_writes_.empty()) return;
    current_write_.emplace(std::move(pending_writes_.front()));
    pending_writes_.pop_front();
    if (logger_) {
      logger_->debug("start write msg_case={} remaining={}",
                     current_write_->msg_case(), pending_writes_.size());
    }
    write_in_flight_ = true;
    reactor_->StartWrite(&*current_write_);
  }

  void EnqueueWrite(ops::v1::AgentToServer msg) {
    if (options_.on_send) {
      options_.on_send(msg);
    }
    std::lock_guard<std::mutex> lock(mu_);
    pending_writes_.push_back(std::move(msg));
    MaybeStartWriteLocked();
  }

  void SendAck(const std::string& op_id, bool accepted, std::string reason = std::string()) {
    ops::v1::AgentToServer msg;
    auto* ack = msg.mutable_ack();
    ack->set_op_id(op_id);
    ack->set_accepted(accepted);
    if (!reason.empty()) {
      ack->set_reason(std::move(reason));
    }
    logger_->debug("send Ack op_id={} accepted={} reason={}", op_id, accepted, reason);
    EnqueueWrite(std::move(msg));
  }

  void DoSendError(const std::string& op_id, std::string code, std::string message) {
    ops::v1::AgentToServer msg;
    auto* err = msg.mutable_error();
    err->set_op_id(op_id);
    err->set_code(std::move(code));
    err->set_message(std::move(message));
    logger_->warn("send Error op_id={} code={} message={}", op_id, err->code(), err->message());
    EnqueueWrite(std::move(msg));
  }

  void DoSendLogData(const std::string& op_id, ops::v1::LogChunk chunk) {
    ops::v1::AgentToServer msg;
    auto* data = msg.mutable_data();
    data->set_op_id(op_id);
    data->mutable_log_chunk()->Swap(&chunk);
    EnqueueWrite(std::move(msg));
  }

  void DoSendPcapData(const std::string& op_id, ops::v1::PcapPacket pkt) {
    ops::v1::AgentToServer msg;
    auto* data = msg.mutable_data();
    data->set_op_id(op_id);
    data->mutable_pcap_packet()->Swap(&pkt);
    EnqueueWrite(std::move(msg));
  }

  void DoSendEofLog(const std::string& op_id, const ops::v1::LogFilterEof& eof) {
    ops::v1::AgentToServer msg;
    auto* tail = msg.mutable_eof();
    tail->set_op_id(op_id);
    tail->mutable_log()->CopyFrom(eof);
    logger_->info("log filter op {} completed size={} lines={}", op_id, eof.total_size(), eof.total_lines());
    EnqueueWrite(std::move(msg));
  }

  void DoSendEofPcap(const std::string& op_id, const ops::v1::PcapStats& stats) {
    ops::v1::AgentToServer msg;
    auto* tail = msg.mutable_eof();
    tail->set_op_id(op_id);
    tail->mutable_pcap()->CopyFrom(stats);
    logger_->info("pcap op {} completed packets={} dropped={}", op_id, stats.received(), stats.dropped());
    EnqueueWrite(std::move(msg));
  }

  void HandleStartOp(const ops::v1::StartOp& start) {
    const std::string op_id = start.meta().op_id();
    if (op_id.empty()) {
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
        task = std::make_shared<LogFilterTask>(op_id, start.log_filter(), options_.log_options, logger_);
      } else if (start.has_pcap()) {
        task = std::make_shared<PcapTask>(op_id, start.pcap(), logger_);
      } else {
        if (logger_) {
          logger_->warn("reject StartOp op_id={} reason=unsupported", op_id);
        }
        SendAck(op_id, false, "unsupported operation");
        return;
      }

      tasks_[op_id] = task;
      task_queue_.push_back(task);
    }

    const char* type_name = task->kind() == Task::Kind::kLogFilter ? "log" : "pcap";
    logger_->info("accepted StartOp type={} op_id={}", type_name, op_id);
    SendAck(op_id, true);
    task_cv_.notify_all();
  }

  void HandleCancel(const std::string& op_id) {
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
    ControlStreamReactor* reactor_to_cancel = nullptr;
    std::vector<std::string> cancelled_ops;
    {
      std::lock_guard<std::mutex> lock(mu_);
      drain_mode_ = drain;
      logger_->info("received shutdown request drain={} queue_size={}", drain, task_queue_.size());
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
        reactor_to_cancel = reactor_;
      }
    }
    if (reactor_to_cancel) {
      reactor_to_cancel->TryCancel();
    }
    if (!drain) {
      for (const auto& op : cancelled_ops) {
        DoSendError(op, "CANCELLED", "operation cancelled");
      }
      running_.store(false);
      task_cv_.notify_all();
    }
  }

  void CancelAllLocked(std::string_view reason) {
    if (logger_) {
      logger_->warn("cancelling all tasks reason={} queue_size={} current={}", reason,
                    task_queue_.size(), current_task_ ? current_task_->op_id() : "none");
    }
    std::vector<std::string> cancelled_ops;
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
    if (reactor_) {
      for (const auto& op_id : cancelled_ops) {
        ops::v1::AgentToServer msg;
        auto* err = msg.mutable_error();
        err->set_op_id(op_id);
        err->set_code("CANCELLED");
        err->set_message(std::string(reason));
        pending_writes_.push_back(std::move(msg));
      }
      MaybeStartWriteLocked();
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
          const char* type_name = task->kind() == Task::Kind::kLogFilter ? "log" : "pcap";
          logger_->info("starting task op_id={} type={}", task->op_id(), type_name);
        }
      }

      RunTask(task);

      {
        std::lock_guard<std::mutex> lock(mu_);
        if (task) {
          tasks_.erase(task->op_id());
          if (logger_) {
            const char* type_name = task->kind() == Task::Kind::kLogFilter ? "log" : "pcap";
            logger_->info("task finished op_id={} type={} state={}"
                          , task->op_id(), type_name, static_cast<int>(task->state()));
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

  std::atomic<bool> running_{false};
  std::thread worker_thread_;

  std::mutex mu_;
  ControlStreamReactor* reactor_ = nullptr;
  std::deque<ops::v1::AgentToServer> pending_writes_;
  std::optional<ops::v1::AgentToServer> current_write_;
  bool write_in_flight_ = false;

  std::condition_variable task_cv_;
  std::deque<std::shared_ptr<Task>> task_queue_;
  std::unordered_map<std::string, std::shared_ptr<Task>> tasks_;
  std::shared_ptr<Task> current_task_;
  bool drain_mode_ = false;
  bool stop_worker_ = false;
  std::shared_ptr<spdlog::logger> logger_;
};

ControlStreamReactor::ControlStreamReactor(ControlCallbackClient* parent,
                                           ops::v1::Control::StubInterface* stub,
                                           grpc::ClientContext* ctx)
    : parent_(parent), context_(ctx) {
  stub->experimental_async()->Connect(context_, this);
}

void ControlStreamReactor::Begin() {
  StartCall();
  parent_->OnStreamReady(this);
  StartRead(&incoming_);
}

grpc::Status ControlStreamReactor::Wait() {
  std::unique_lock<std::mutex> lock(mu_);
  cv_.wait(lock, [&] { return done_; });
  return status_;
}

void ControlStreamReactor::InjectMessage(const ops::v1::ServerToAgent& msg) {
  incoming_ = msg;
  OnReadDone(true);
}

void ControlStreamReactor::OnReadDone(bool ok) {
  parent_->OnMessage(incoming_, ok);
  if (ok) {
    StartRead(&incoming_);
  }
}

void ControlStreamReactor::OnWriteDone(bool ok) {
  parent_->OnWriteFinished(ok);
}

void ControlStreamReactor::OnDone(const ::grpc::Status& status) {
  parent_->OnStreamClosed(status);
  {
    std::lock_guard<std::mutex> lock(mu_);
    done_ = true;
    status_ = status;
  }
  cv_.notify_all();
}

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

  ControlCallbackClient client(stub, agent_id, options);
  client.Run();
}

}  // namespace zurg::agent
