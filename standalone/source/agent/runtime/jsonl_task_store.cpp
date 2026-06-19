#include "runtime/jsonl_task_store.h"

#include <algorithm>
#include <fstream>
#include <system_error>

#include <nlohmann/json.hpp>

namespace zurg::agent::runtime {
namespace {

using json = nlohmann::json;

bool IsTerminal(tasks::Task::State state) {
  return state == tasks::Task::State::kCompleted ||
         state == tasks::Task::State::kCancelled ||
         state == tasks::Task::State::kFailed;
}

json EventToJson(const TaskEvent& event) {
  json out;
  out["sequence"] = event.sequence;
  out["op_id"] = event.op_id;
  out["task_kind"] = ToString(event.task_kind);
  out["source"] = ToString(event.source);
  out["event_kind"] = ToString(event.event_kind);
  out["payload_kind"] = ToString(event.payload_kind);
  out["state"] = ToString(event.state);
  out["accepted"] = event.accepted;
  out["code"] = event.code;
  out["message"] = event.message;
  out["timestamp_ms"] = ToUnixMillis(event.timestamp);
  if (event.payload) {
    out["payload"] = {{"path", event.payload->path.string()},
                      {"size_bytes", event.payload->size_bytes}};
  }
  return out;
}

TaskEvent EventFromJson(const json& input) {
  TaskEvent event;
  event.sequence = input.value("sequence", 0ULL);
  event.op_id = input.value("op_id", 0U);
  event.task_kind = TaskKindFromString(input.value("task_kind", "log_filter"));
  event.source = TaskInputSourceFromString(input.value("source", "recovered"));
  event.event_kind = TaskEventKindFromString(input.value("event_kind", "state_changed"));
  event.payload_kind = TaskPayloadKindFromString(input.value("payload_kind", "none"));
  event.state = TaskStateFromString(input.value("state", "pending"));
  event.accepted = input.value("accepted", false);
  event.code = input.value("code", "");
  event.message = input.value("message", "");
  event.timestamp = FromUnixMillis(input.value("timestamp_ms", 0LL));
  if (input.contains("payload")) {
    PayloadRef ref;
    ref.path = input["payload"].value("path", "");
    ref.size_bytes = input["payload"].value("size_bytes", 0ULL);
    event.payload = std::move(ref);
  }
  return event;
}

}  // namespace

JsonlTaskStore::JsonlTaskStore(std::filesystem::path root,
                               std::shared_ptr<spdlog::logger> logger)
    : root_(std::move(root)), logger_(std::move(logger)) {}

bool JsonlTaskStore::Open(std::string* error) {
  std::lock_guard<std::mutex> lock(mu_);
  std::error_code ec;
  std::filesystem::create_directories(root_, ec);
  if (ec) {
    if (error) *error = ec.message();
    return false;
  }
  payload_dir_ = root_ / "payloads";
  std::filesystem::create_directories(payload_dir_, ec);
  if (ec) {
    if (error) *error = ec.message();
    return false;
  }
  events_path_ = root_ / "events.jsonl";

  std::ifstream in(events_path_);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    try {
      auto event = EventFromJson(json::parse(line));
      next_sequence_ = std::max(next_sequence_, event.sequence + 1);
      ApplyEventLocked(event);
      events_[event.op_id].push_back(std::move(event));
    } catch (const std::exception& ex) {
      if (logger_) {
        logger_->warn("skipping invalid task store record: {}", ex.what());
      }
    }
  }
  MarkInterruptedTasksLocked();
  return true;
}

void JsonlTaskStore::OnTaskEvent(const TaskEvent& input) {
  std::lock_guard<std::mutex> lock(mu_);
  TaskEvent event = input;
  if (event.sequence == 0) {
    event.sequence = next_sequence_++;
  } else {
    next_sequence_ = std::max(next_sequence_, event.sequence + 1);
  }

  if (!event.payload_bytes.empty() && !event.payload) {
    auto payload_path = PayloadPath(event);
    std::ofstream payload(payload_path, std::ios::binary | std::ios::trunc);
    if (!payload) {
      if (logger_) {
        logger_->error("failed to create task payload {}", payload_path.string());
      }
      return;
    }
    payload.write(event.payload_bytes.data(), static_cast<std::streamsize>(event.payload_bytes.size()));
    if (!payload) {
      if (logger_) {
        logger_->error("failed to write task payload {}", payload_path.string());
      }
      return;
    }
    PayloadRef ref;
    ref.path = std::filesystem::relative(payload_path, root_);
    ref.size_bytes = event.payload_bytes.size();
    event.payload = std::move(ref);
  }

  std::ofstream out(events_path_, std::ios::app);
  if (!out) {
    if (logger_) {
      logger_->error("failed to open task store {}", events_path_.string());
    }
    return;
  }
  out << EventToJson(event).dump() << '\n';
  if (!out && logger_) {
    logger_->error("failed to append task event op_id={}", event.op_id);
    return;
  }
  ApplyEventLocked(event);
  events_[event.op_id].push_back(std::move(event));
}

std::vector<StoredTask> JsonlTaskStore::ListTasks() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<StoredTask> out;
  out.reserve(tasks_.size());
  for (const auto& [_, task] : tasks_) {
    out.push_back(task);
  }
  std::sort(out.begin(), out.end(), [](const StoredTask& lhs, const StoredTask& rhs) {
    return lhs.created_at_ms < rhs.created_at_ms;
  });
  return out;
}

std::optional<StoredTask> JsonlTaskStore::GetTask(std::uint32_t op_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = tasks_.find(op_id);
  if (it == tasks_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::vector<TaskEvent> JsonlTaskStore::ListEvents(std::uint32_t op_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = events_.find(op_id);
  if (it == events_.end()) {
    return {};
  }
  return it->second;
}

std::filesystem::path JsonlTaskStore::PayloadPath(const TaskEvent& event) const {
  return payload_dir_ / (std::to_string(event.op_id) + "-" +
                         std::to_string(event.sequence) + ".bin");
}

void JsonlTaskStore::ApplyEventLocked(const TaskEvent& event) {
  auto& task = tasks_[event.op_id];
  if (task.op_id == 0) {
    task.op_id = event.op_id;
    task.task_kind = event.task_kind;
    task.source = event.source;
    task.state = tasks::Task::State::kPending;
    task.created_at_ms = ToUnixMillis(event.timestamp);
  }
  task.updated_at_ms = ToUnixMillis(event.timestamp);
  task.task_kind = event.task_kind;
  task.source = event.source;

  if (event.event_kind == TaskEventKind::kAccepted) {
    task.state = tasks::Task::State::kPending;
  } else if (event.event_kind == TaskEventKind::kRejected) {
    task.state = tasks::Task::State::kFailed;
    task.error_message = event.message;
  } else if (event.event_kind == TaskEventKind::kError) {
    task.state = event.state == tasks::Task::State::kCancelled ? event.state
                                                               : tasks::Task::State::kFailed;
    task.error_code = event.code;
    task.error_message = event.message;
  } else if (event.event_kind == TaskEventKind::kEof) {
    task.state = tasks::Task::State::kCompleted;
  } else if (event.event_kind == TaskEventKind::kStateChanged) {
    task.state = event.state;
  }
}

void JsonlTaskStore::MarkInterruptedTasksLocked() {
  for (auto& [op_id, task] : tasks_) {
    if (IsTerminal(task.state)) {
      continue;
    }
    task.state = tasks::Task::State::kFailed;
    task.error_code = "INTERRUPTED";
    task.error_message = "agent restarted before task reached a terminal state";
    task.updated_at_ms = ToUnixMillis(std::chrono::system_clock::now());
    TaskEvent event;
    event.sequence = next_sequence_++;
    event.op_id = op_id;
    event.task_kind = task.task_kind;
    event.source = TaskInputSource::kRecovered;
    event.event_kind = TaskEventKind::kError;
    event.state = task.state;
    event.code = task.error_code;
    event.message = task.error_message;
    event.timestamp = FromUnixMillis(task.updated_at_ms);
    events_[op_id].push_back(event);
    std::ofstream out(events_path_, std::ios::app);
    if (out) {
      out << EventToJson(event).dump() << '\n';
    }
  }
}

}  // namespace zurg::agent::runtime
