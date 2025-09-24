#include "agent_impl.h"

#include <fmt/core.h>

#include "zurg/file_ops.h"
#include "zurg/pcap_ops.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <condition_variable>
#include <deque>
#include <functional>
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

struct PendingTask {
  enum class Type { kFileGet, kPcap };

  std::string op_id;
  Type type;
  ops::v1::StartOp start;
  std::atomic<bool> cancelled{false};
};

class ControlCallbackClient;

class ControlStreamReactor : public grpc::ClientBidiReactor<ops::v1::AgentToServer, ops::v1::ServerToAgent> {
 public:
  ControlStreamReactor(ControlCallbackClient* parent,
                       ops::v1::Control::StubInterface* stub,
                       grpc::ClientContext* ctx);

  void Begin();
  grpc::Status Wait();
  void InjectMessage(const ops::v1::ServerToAgent& msg);

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

class ControlCallbackClient {
 public:
  struct Options {
    zurg::file_ops::Options file_options;
    TaskShouldContinueFn should_run;
    std::function<std::chrono::milliseconds(std::size_t)> backoff_fn;
    std::function<void(std::chrono::milliseconds)> sleep_fn;
    std::function<void(const ops::v1::AgentToServer&)> on_send;
  };

  ControlCallbackClient(ops::v1::Control::StubInterface* stub,
                        std::string agent_id,
                        Options options)
      : stub_(stub), agent_id_(std::move(agent_id)), options_(std::move(options)) {
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
  }

  ~ControlCallbackClient() { Stop(); }

  void Run() {
    running_.store(true);
    worker_thread_ = std::thread(&ControlCallbackClient::WorkerLoop, this);

    std::size_t attempt = 0;
    while (options_.should_run && options_.should_run()) {
      grpc::ClientContext ctx;
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
      options_.sleep_fn(options_.backoff_fn(attempt));
    }

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
    std::lock_guard<std::mutex> lock(mu_);
    reactor_ = reactor;
    pending_writes_.push_back(internal::MakeHello(agent_id_));
    MaybeStartWriteLocked();
  }

  void OnWriteFinished(bool ok) {
    std::lock_guard<std::mutex> lock(mu_);
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

 private:
  void MaybeStartWriteLocked() {
    if (!reactor_ || write_in_flight_ || pending_writes_.empty()) return;
    current_write_.emplace(std::move(pending_writes_.front()));
    pending_writes_.pop_front();
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
    EnqueueWrite(std::move(msg));
  }

  void SendError(const std::string& op_id, std::string code, std::string message) {
    ops::v1::AgentToServer msg;
    auto* err = msg.mutable_error();
    err->set_op_id(op_id);
    err->set_code(std::move(code));
    err->set_message(std::move(message));
    EnqueueWrite(std::move(msg));
  }

  void SendFileData(const std::string& op_id, ops::v1::FileChunk chunk) {
    ops::v1::AgentToServer msg;
    auto* data = msg.mutable_data();
    data->set_op_id(op_id);
    data->mutable_file_chunk()->Swap(&chunk);
    EnqueueWrite(std::move(msg));
  }

  void SendPcapData(const std::string& op_id, ops::v1::PcapPacket pkt) {
    ops::v1::AgentToServer msg;
    auto* data = msg.mutable_data();
    data->set_op_id(op_id);
    data->mutable_pcap_packet()->Swap(&pkt);
    EnqueueWrite(std::move(msg));
  }

  void SendEofFile(const std::string& op_id, const ops::v1::FileGetEof& eof) {
    ops::v1::AgentToServer msg;
    auto* tail = msg.mutable_eof();
    tail->set_op_id(op_id);
    tail->mutable_file()->CopyFrom(eof);
    EnqueueWrite(std::move(msg));
  }

  void SendEofPcap(const std::string& op_id, const ops::v1::PcapStats& stats) {
    ops::v1::AgentToServer msg;
    auto* tail = msg.mutable_eof();
    tail->set_op_id(op_id);
    tail->mutable_pcap()->CopyFrom(stats);
    EnqueueWrite(std::move(msg));
  }

  void HandleStartOp(const ops::v1::StartOp& start) {
    const std::string op_id = start.meta().op_id();
    if (op_id.empty()) {
      SendAck(op_id, false, "missing op_id");
      return;
    }

    std::shared_ptr<PendingTask> task;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (drain_mode_) {
        SendAck(op_id, false, "draining");
        return;
      }
      if (tasks_.count(op_id)) {
        SendAck(op_id, false, "duplicate op_id");
        return;
      }
      PendingTask::Type type;
      if (start.has_file_get()) {
        type = PendingTask::Type::kFileGet;
      } else if (start.has_pcap()) {
        type = PendingTask::Type::kPcap;
      } else {
        SendAck(op_id, false, "unsupported operation");
        return;
      }
      task = std::make_shared<PendingTask>();
      task->op_id = op_id;
      task->type = type;
      task->start = start;
      task_queue_.push_back(task);
      tasks_[op_id] = task;
    }

    SendAck(op_id, true);
    task_cv_.notify_all();
  }

  void HandleCancel(const std::string& op_id) {
    std::shared_ptr<PendingTask> target;
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = tasks_.find(op_id);
      if (it == tasks_.end()) {
        return;
      }
      target = it->second.lock();
      if (!target) {
        tasks_.erase(it);
        return;
      }
      if (current_task_ && current_task_->op_id == op_id) {
        target->cancelled.store(true);
        return;
      }
      auto q_it = std::find_if(task_queue_.begin(), task_queue_.end(),
                               [&](const std::shared_ptr<PendingTask>& t) { return t->op_id == op_id; });
      if (q_it != task_queue_.end()) {
        task_queue_.erase(q_it);
        tasks_.erase(op_id);
      } else {
        return;
      }
    }
    if (target) {
      target->cancelled.store(true);
      SendError(op_id, "CANCELLED", "operation cancelled");
    }
  }

  void HandleShutdown(const ops::v1::Shutdown& shutdown) {
    const bool drain = shutdown.drain();
    {
      std::lock_guard<std::mutex> lock(mu_);
      drain_mode_ = drain;
      if (!drain) {
        CancelAllLocked("shutdown");
      }
    }
    if (!drain) {
      running_.store(false);
      task_cv_.notify_all();
    }
  }

  void CancelAllLocked(std::string_view reason) {
    for (auto& entry : tasks_) {
      if (auto ptr = entry.second.lock()) {
        ptr->cancelled.store(true);
        ops::v1::AgentToServer msg;
        auto* err = msg.mutable_error();
        err->set_op_id(ptr->op_id);
        err->set_code("CANCELLED");
        err->set_message(std::string(reason));
        pending_writes_.push_back(std::move(msg));
      }
    }
    tasks_.clear();
    task_queue_.clear();
    MaybeStartWriteLocked();
  }

  bool ShouldStop(const std::shared_ptr<PendingTask>& task) const {
    return task->cancelled.load() || (options_.should_run && !options_.should_run());
  }

  void RunFileTask(const std::shared_ptr<PendingTask>& task) {
    ops::v1::FileGetEof eof;
    auto consumer = [this, task](ops::v1::FileChunk chunk) -> ::grpc::Status {
      if (task->cancelled.load()) {
        return ::grpc::Status(::grpc::StatusCode::CANCELLED, "cancelled");
      }
      SendFileData(task->op_id, std::move(chunk));
      return ::grpc::Status::OK;
    };
    auto should_stop = [this, task]() { return ShouldStop(task); };
    ::grpc::Status status = zurg::file_ops::StreamFile(options_.file_options,
                                                      task->start.file_get(),
                                                      should_stop,
                                                      consumer,
                                                      &eof);
    if (status.ok()) {
      SendEofFile(task->op_id, eof);
    } else {
      SendError(task->op_id, std::to_string(static_cast<int>(status.error_code())), status.error_message());
    }
  }

  void RunPcapTask(const std::shared_ptr<PendingTask>& task) {
    ops::v1::PcapStats stats;
    auto consumer = [this, task](ops::v1::PcapPacket pkt) -> ::grpc::Status {
      if (task->cancelled.load()) {
        return ::grpc::Status(::grpc::StatusCode::CANCELLED, "cancelled");
      }
      SendPcapData(task->op_id, std::move(pkt));
      return ::grpc::Status::OK;
    };
    auto should_stop = [this, task]() { return ShouldStop(task); };
    ::grpc::Status status = zurg::pcap_ops::StreamCapture(task->start.pcap(), consumer, &stats, should_stop);
    if (status.ok()) {
      SendEofPcap(task->op_id, stats);
    } else {
      SendError(task->op_id, std::to_string(static_cast<int>(status.error_code())), status.error_message());
    }
  }

  void RunTask(const std::shared_ptr<PendingTask>& task) {
    switch (task->type) {
      case PendingTask::Type::kFileGet:
        RunFileTask(task);
        break;
      case PendingTask::Type::kPcap:
        RunPcapTask(task);
        break;
    }
  }

  void WorkerLoop() {
    while (running_.load()) {
      std::shared_ptr<PendingTask> task;
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
      }

      RunTask(task);

      {
        std::lock_guard<std::mutex> lock(mu_);
        tasks_.erase(task->op_id);
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
  std::deque<std::shared_ptr<PendingTask>> task_queue_;
  std::unordered_map<std::string, std::weak_ptr<PendingTask>> tasks_;
  std::shared_ptr<PendingTask> current_task_;
  bool drain_mode_ = false;
  bool stop_worker_ = false;
};

ControlStreamReactor::ControlStreamReactor(ControlCallbackClient* parent,
                                           ops::v1::Control::StubInterface* stub,
                                           grpc::ClientContext* ctx)
    : parent_(parent), context_(ctx) {
  stub->experimental_async()->Connect(context_, this);
}

void ControlStreamReactor::Begin() {
  parent_->OnStreamReady(this);
  StartRead(&incoming_);
  StartCall();
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
    fmt::print(stderr, "[agent] missing stub, aborting\n");
    return;
  }

  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);

  ControlCallbackClient::Options options;
  options.should_run = [] { return internal::IsRunning(); };
  options.backoff_fn = [](std::size_t attempt) { return internal::ComputeBackoff(attempt); };
  options.sleep_fn = [](std::chrono::milliseconds delay) { internal::SleepWithStop(delay); };

  ControlCallbackClient client(stub, agent_id, options);
  client.Run();
}

}  // namespace zurg::agent
