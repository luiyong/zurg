#include "control/control_callback_client.h"

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <utility>
#include <vector>
#include <string_view>

#include <spdlog/spdlog.h>

#include "agent/agent_impl.h"
#include "tasks/exec_task.h"
#include "tasks/log_filter_task.h"
#include "tasks/pcap_task.h"

namespace zurg::agent {

namespace {

constexpr std::string_view kAuthorizationDisabledReason = "authorization disabled";
constexpr std::string_view kFeaturesDisabledReason = "features disabled";
constexpr std::string_view kLogFilterDisabledReason = "log filter disabled";
constexpr std::string_view kPcapDisabledReason = "pcap disabled";
constexpr std::string_view kExecDisabledReason = "exec disabled";
constexpr std::string_view kAuthorizationErrorMessage =
    "operation disabled by authorization state";

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

}  // namespace

ControlCallbackClient::ControlCallbackClient(ops::v1::Control::StubInterface* stub,
                                             std::string agent_id, Options options)
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

  requested_features_ = options_.features;
  active_features_ = options_.features;
  auth_subscription_ = events::GlobalEventBus().Subscribe<events::AuthStateChangedEvent>(
      [this](const events::AuthStateChangedEvent& event) { OnAuthStateChanged(event); });

  ControlStreamClient::Options stream_options;
  stream_options.should_run = [this] { return ShouldStreamContinue(); };
  stream_options.backoff_fn = options_.backoff_fn;
  stream_options.sleep_fn = options_.sleep_fn;
  stream_options.on_send = options_.on_send;
  stream_client_ =
      std::make_unique<ControlStreamClient>(stub_, std::move(stream_options), logger_);
  stream_client_->SetReadyCallback([this] { EnqueueWrite(MakeHello(agent_id_)); });
  stream_client_->SetMessageCallback(
      [this](const ops::v1::ServerToAgent& msg, bool ok) { HandleStreamMessage(msg, ok); });
  stream_client_->SetStreamClosedCallback(
      [this](const grpc::Status& status) { HandleStreamClosed(status); });

  if (auto last = events::GlobalEventBus().LastEvent<events::AuthStateChangedEvent>()) {
    OnAuthStateChanged(*last);
  } else {
    ApplyFeatureUpdate(active_features_);
  }
}

ControlCallbackClient::~ControlCallbackClient() {
  Stop();
  auth_subscription_.Unsubscribe();
}

bool ControlCallbackClient::ShouldContinue() const { return ShouldContinueInternal(); }

void ControlCallbackClient::SendLogData(std::uint32_t op_id, ops::v1::LogChunk chunk) {
  DoSendLogData(op_id, std::move(chunk));
}

void ControlCallbackClient::SendEofLog(std::uint32_t op_id, const ops::v1::LogFilterEof& eof) {
  DoSendEofLog(op_id, eof);
}

void ControlCallbackClient::SendPcapData(std::uint32_t op_id, ops::v1::PcapPacket pkt) {
  DoSendPcapData(op_id, std::move(pkt));
}

void ControlCallbackClient::SendEofPcap(std::uint32_t op_id, const ops::v1::PcapStats& stats) {
  DoSendEofPcap(op_id, stats);
}

void ControlCallbackClient::SendError(std::uint32_t op_id, std::string code,
                                      std::string message) {
  DoSendError(op_id, std::move(code), std::move(message));
}

void ControlCallbackClient::SendExecData(std::uint32_t op_id, ops::v1::ExecChunk chunk) {
  DoSendExecData(op_id, std::move(chunk));
}

void ControlCallbackClient::SendEofExec(std::uint32_t op_id, const ops::v1::ExecExit& exit) {
  DoSendEofExec(op_id, exit);
}

void ControlCallbackClient::Run() {
  logger_->info("starting callback client for agent {}", agent_id_);
  running_.store(true);
  worker_thread_ = std::thread(&ControlCallbackClient::WorkerLoop, this);

  stream_client_->Run();

  running_.store(false);
  ShutdownWorker();
  logger_->info("stopping callback client for agent {}", agent_id_);
}

void ControlCallbackClient::Stop() {
  bool expected = true;
  if (!running_.compare_exchange_strong(expected, false)) {
    running_.store(false);
  }
  if (stream_client_) {
    stream_client_->Stop();
  }
  ShutdownWorker();
  auth_subscription_.Unsubscribe();
}

bool ControlCallbackClient::ShouldStreamContinue() const {
  if (!running_.load()) {
    return false;
  }
  if (options_.should_run && !options_.should_run()) {
    return false;
  }
  return true;
}

void ControlCallbackClient::ShutdownWorker() {
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

bool ControlCallbackClient::ShouldContinueInternal() const {
  if (!running_.load()) return false;
  if (!options_.should_run) return true;
  return options_.should_run();
}

void ControlCallbackClient::EnqueueWrite(ops::v1::AgentToServer msg) {
  stream_client_->EnqueueWrite(std::move(msg));
}

void ControlCallbackClient::SendAck(std::uint32_t op_id, bool accepted, std::string reason) {
  ops::v1::AgentToServer msg;
  auto* ack = msg.mutable_ack();
  ack->set_op_id(op_id);
  ack->set_accepted(accepted);
  if (!reason.empty()) {
    ack->set_reason(std::move(reason));
  }
  if (logger_) {
    logger_->debug("send Ack op_id={} accepted={} reason={}", op_id, accepted, ack->reason());
  }
  EnqueueWrite(std::move(msg));
}

void ControlCallbackClient::DoSendError(std::uint32_t op_id, std::string code,
                                        std::string message) {
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

void ControlCallbackClient::DoSendLogData(std::uint32_t op_id, ops::v1::LogChunk chunk) {
  ops::v1::AgentToServer msg;
  auto* data = msg.mutable_data();
  data->set_op_id(op_id);
  data->mutable_log_chunk()->Swap(&chunk);
  EnqueueWrite(std::move(msg));
}

void ControlCallbackClient::DoSendPcapData(std::uint32_t op_id, ops::v1::PcapPacket pkt) {
  ops::v1::AgentToServer msg;
  auto* data = msg.mutable_data();
  data->set_op_id(op_id);
  data->mutable_pcap_packet()->Swap(&pkt);
  EnqueueWrite(std::move(msg));
}

void ControlCallbackClient::DoSendEofLog(std::uint32_t op_id, const ops::v1::LogFilterEof& eof) {
  ops::v1::AgentToServer msg;
  auto* tail = msg.mutable_eof();
  tail->set_op_id(op_id);
  tail->mutable_log()->CopyFrom(eof);
  if (logger_) {
    logger_->info("log filter op {} completed size={} lines={}", op_id, eof.total_size(),
                  eof.total_lines());
  }
  EnqueueWrite(std::move(msg));
}

void ControlCallbackClient::DoSendEofPcap(std::uint32_t op_id, const ops::v1::PcapStats& stats) {
  ops::v1::AgentToServer msg;
  auto* tail = msg.mutable_eof();
  tail->set_op_id(op_id);
  tail->mutable_pcap()->CopyFrom(stats);
  if (logger_) {
    logger_->info("pcap op {} completed packets={} dropped={}", op_id, stats.received(),
                  stats.dropped());
  }
  EnqueueWrite(std::move(msg));
}

void ControlCallbackClient::DoSendExecData(std::uint32_t op_id, ops::v1::ExecChunk chunk) {
  ops::v1::AgentToServer msg;
  auto* data = msg.mutable_data();
  data->set_op_id(op_id);
  data->mutable_exec_chunk()->Swap(&chunk);
  EnqueueWrite(std::move(msg));
}

void ControlCallbackClient::DoSendEofExec(std::uint32_t op_id, const ops::v1::ExecExit& exit) {
  ops::v1::AgentToServer msg;
  auto* tail = msg.mutable_eof();
  tail->set_op_id(op_id);
  tail->mutable_exec()->CopyFrom(exit);
  if (logger_) {
    logger_->info("exec op {} completed code={} note={}", op_id, exit.code(), exit.note());
  }
  EnqueueWrite(std::move(msg));
}

void ControlCallbackClient::OnAuthStateChanged(const events::AuthStateChangedEvent& event) {
  if (logger_) {
    logger_->info("authorization state changed to {}", event.state == auth::AuthState::kOnline
                                                          ? "ONLINE"
                                                          : (event.state == auth::AuthState::kOffline
                                                                 ? "OFFLINE"
                                                                 : "UNKNOWN"));
  }
  if (event.state == auth::AuthState::kOnline) {
    ApplyFeatureUpdate(requested_features_);
  } else {
    ApplyFeatureUpdate(RestrictedFeatures());
  }
}

void ControlCallbackClient::ApplyFeatureUpdate(const FeatureToggles& toggles) {
  std::vector<std::uint32_t> to_error;
  bool notify_worker = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (active_features_.enabled == toggles.enabled &&
        active_features_.enable_log_filter == toggles.enable_log_filter &&
        active_features_.enable_pcap == toggles.enable_pcap &&
        active_features_.enable_exec == toggles.enable_exec) {
      return;
    }

    active_features_ = toggles;
    if (logger_) {
      logger_->info("active feature toggles updated: enabled={} log_filter={} pcap={} exec={}",
                    active_features_.enabled ? "on" : "off",
                    active_features_.enable_log_filter ? "on" : "off",
                    active_features_.enable_pcap ? "on" : "off",
                    active_features_.enable_exec ? "on" : "off");
    }

    auto is_allowed = [&](const std::shared_ptr<tasks::Task>& task) {
      if (!task) {
        return false;
      }
      return IsTaskAllowed(active_features_, task->kind());
    };

    for (auto it = task_queue_.begin(); it != task_queue_.end();) {
      const auto& task = *it;
      if (!task || !is_allowed(task)) {
        if (task) {
          to_error.push_back(task->op_id());
          tasks_.erase(task->op_id());
        }
        it = task_queue_.erase(it);
        notify_worker = true;
      } else {
        ++it;
      }
    }

    for (auto it = tasks_.begin(); it != tasks_.end();) {
      auto task = it->second;
      if (!task) {
        it = tasks_.erase(it);
        continue;
      }
      if (!IsTaskAllowed(active_features_, task->kind())) {
        if (current_task_ && task == current_task_) {
          task->RequestCancel();
          ++it;
          continue;
        }
        to_error.push_back(task->op_id());
        it = tasks_.erase(it);
        notify_worker = true;
      } else {
        ++it;
      }
    }

    if (current_task_ && !IsTaskAllowed(active_features_, current_task_->kind())) {
      current_task_->RequestCancel();
      notify_worker = true;
    }
  }

  if (notify_worker) {
    task_cv_.notify_all();
  }
  for (auto op_id : to_error) {
    DoSendError(op_id, "UNAUTHORIZED", std::string(kAuthorizationErrorMessage));
  }
}

bool ControlCallbackClient::IsTaskAllowed(const FeatureToggles& toggles,
                                          tasks::Task::Kind kind) const {
  if (!toggles.enabled) {
    return false;
  }
  switch (kind) {
    case tasks::Task::Kind::kLogFilter:
      return toggles.enable_log_filter;
    case tasks::Task::Kind::kPcap:
      return toggles.enable_pcap;
    case tasks::Task::Kind::kExec:
      return toggles.enable_exec;
  }
  return false;
}

FeatureToggles ControlCallbackClient::RestrictedFeatures() const {
  FeatureToggles toggles{};
  toggles.enabled = false;
  toggles.enable_log_filter = false;
  toggles.enable_pcap = false;
  toggles.enable_exec = false;
  return toggles;
}

void ControlCallbackClient::HandleStreamMessage(const ops::v1::ServerToAgent& msg, bool ok) {
  if (!ok) {
    return;
  }
  switch (msg.msg_case()) {
    case ops::v1::ServerToAgent::kPing:
      EnqueueWrite(MakePong(agent_id_, msg.ping()));
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

void ControlCallbackClient::HandleStreamClosed(const grpc::Status&) {
  std::lock_guard<std::mutex> lock(mu_);
  CancelAllLocked("stream closed");
}

void ControlCallbackClient::HandleStartOp(const ops::v1::StartOp& start) {
  const std::uint32_t op_id = start.meta().op_id();
  if (op_id == 0) {
    if (logger_) {
      logger_->warn("received StartOp with missing op_id");
    }
    SendAck(op_id, false, "missing op_id");
    return;
  }

  std::shared_ptr<tasks::Task> task;
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

    FeatureToggles features_snapshot = active_features_;
    if (!features_snapshot.enabled) {
      const auto& reason = requested_features_.enabled ? kAuthorizationDisabledReason
                                                       : kFeaturesDisabledReason;
      if (logger_) {
        logger_->warn("reject StartOp op_id={} reason={}", op_id, reason);
      }
      SendAck(op_id, false, std::string(reason));
      return;
    }

    if (start.has_log_filter()) {
      if (!requested_features_.enable_log_filter) {
        if (logger_) {
          logger_->warn("reject StartOp op_id={} reason=log_filter_disabled", op_id);
        }
        SendAck(op_id, false, std::string(kLogFilterDisabledReason));
        return;
      }
      if (!features_snapshot.enable_log_filter) {
        if (logger_) {
          logger_->warn("reject StartOp op_id={} reason=authorization_disabled_log", op_id);
        }
        SendAck(op_id, false, std::string(kAuthorizationDisabledReason));
        return;
      }
      task = std::make_shared<tasks::LogFilterTask>(op_id, start.log_filter(),
                                                    options_.log_options, logger_);
    } else if (start.has_pcap()) {
      if (!requested_features_.enable_pcap) {
        if (logger_) {
          logger_->warn("reject StartOp op_id={} reason=pcap_disabled", op_id);
        }
        SendAck(op_id, false, std::string(kPcapDisabledReason));
        return;
      }
      if (!features_snapshot.enable_pcap) {
        if (logger_) {
          logger_->warn("reject StartOp op_id={} reason=authorization_disabled_pcap", op_id);
        }
        SendAck(op_id, false, std::string(kAuthorizationDisabledReason));
        return;
      }
      task = std::make_shared<tasks::PcapTask>(op_id, start.pcap(), options_.log_options, logger_);
    } else if (start.has_exec()) {
      if (!requested_features_.enable_exec) {
        if (logger_) {
          logger_->warn("reject StartOp op_id={} reason=exec_disabled", op_id);
        }
        SendAck(op_id, false, std::string(kExecDisabledReason));
        return;
      }
      if (!features_snapshot.enable_exec) {
        if (logger_) {
          logger_->warn("reject StartOp op_id={} reason=authorization_disabled_exec", op_id);
        }
        SendAck(op_id, false, std::string(kAuthorizationDisabledReason));
        return;
      }
      task = std::make_shared<tasks::ExecTask>(op_id, start.exec(), logger_);
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
    SendAck(op_id, false,
            validation_error.empty() ? "invalid parameters" : std::move(validation_error));
    return;
  }

  bool accepted = false;
  std::string late_reject_reason;
  {
    std::lock_guard<std::mutex> lock(mu_);
    FeatureToggles features_snapshot = active_features_;
    if (!IsTaskAllowed(features_snapshot, task->kind())) {
      if (!features_snapshot.enabled && requested_features_.enabled) {
        late_reject_reason = std::string(kAuthorizationDisabledReason);
      } else {
        late_reject_reason = std::string(kFeaturesDisabledReason);
      }
    } else {
      tasks_[op_id] = task;
      task_queue_.push_back(task);
      accepted = true;
    }
  }

  if (!accepted) {
    if (late_reject_reason.empty()) {
      late_reject_reason = std::string(kAuthorizationDisabledReason);
    }
    if (logger_) {
      logger_->warn("reject StartOp op_id={} reason={} (post-validation)", op_id,
                    late_reject_reason);
    }
    SendAck(op_id, false, std::move(late_reject_reason));
    return;
  }

  const char* type_name = "unknown";
  switch (task->kind()) {
    case tasks::Task::Kind::kLogFilter:
      type_name = "log";
      break;
    case tasks::Task::Kind::kPcap:
      type_name = "pcap";
      break;
    case tasks::Task::Kind::kExec:
      type_name = "exec";
      break;
  }
  if (logger_) {
    logger_->info("accepted StartOp type={} op_id={}", type_name, op_id);
  }
  SendAck(op_id, true);
  task_cv_.notify_all();
}

void ControlCallbackClient::HandleCancel(std::uint32_t op_id) {
  std::shared_ptr<tasks::Task> target;
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

void ControlCallbackClient::HandleShutdown(const ops::v1::Shutdown& shutdown) {
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

void ControlCallbackClient::CancelAllLocked(std::string_view reason) {
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

void ControlCallbackClient::RunTask(const std::shared_ptr<tasks::Task>& task) {
  if (task) {
    task->Run(*this);
  }
}

void ControlCallbackClient::WorkerLoop() {
  while (running_.load()) {
    std::shared_ptr<tasks::Task> task;
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
          case tasks::Task::Kind::kLogFilter:
            type_name = "log";
            break;
          case tasks::Task::Kind::kPcap:
            type_name = "pcap";
            break;
          case tasks::Task::Kind::kExec:
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
            case tasks::Task::Kind::kLogFilter:
              type_name = "log";
              break;
            case tasks::Task::Kind::kPcap:
              type_name = "pcap";
              break;
            case tasks::Task::Kind::kExec:
              type_name = "exec";
              break;
          }
          logger_->info("task finished op_id={} type={} state={}", task->op_id(), type_name,
                        static_cast<int>(task->state()));
        }
      }
      current_task_.reset();
      if (drain_mode_ && task_queue_.empty()) {
        running_.store(false);
      }
    }
  }
}

}  // namespace zurg::agent
