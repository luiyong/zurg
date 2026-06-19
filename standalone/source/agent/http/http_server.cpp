#include "agent/http/http_server.h"

#include <utility>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "agent/system/process_info.h"

namespace zurg::agent::http {
namespace {

using json = nlohmann::json;

json TaskToJson(const runtime::StoredTask& task) {
  return json{{"op_id", task.op_id},
              {"task_kind", runtime::ToString(task.task_kind)},
              {"source", runtime::ToString(task.source)},
              {"state", runtime::ToString(task.state)},
              {"created_at_ms", task.created_at_ms},
              {"updated_at_ms", task.updated_at_ms},
              {"error_code", task.error_code},
              {"error_message", task.error_message}};
}

json EventToJson(const runtime::TaskEvent& event) {
  json out{{"sequence", event.sequence},
           {"op_id", event.op_id},
           {"task_kind", runtime::ToString(event.task_kind)},
           {"source", runtime::ToString(event.source)},
           {"event_kind", runtime::ToString(event.event_kind)},
           {"payload_kind", runtime::ToString(event.payload_kind)},
           {"state", runtime::ToString(event.state)},
           {"accepted", event.accepted},
           {"code", event.code},
           {"message", event.message},
           {"timestamp_ms", runtime::ToUnixMillis(event.timestamp)}};
  if (event.payload) {
    out["payload"] = {{"path", event.payload->path.string()},
                      {"size_bytes", event.payload->size_bytes}};
  }
  return out;
}

void WriteJson(httplib::Response& res, int status, const json& body) {
  res.status = status;
  res.set_content(body.dump(), "application/json; charset=utf-8");
}

runtime::TaskRequest ParseTaskRequest(const json& body) {
  runtime::TaskRequest request;
  request.source = runtime::TaskInputSource::kHttp;
  request.op_id = body.value("op_id", 0U);
  const auto type = body.value("type", "");
  if (type == "log_filter") {
    ops::v1::LogFilterSpec spec;
    if (body.contains("grep_contains")) {
      spec.set_grep_contains(body.value("grep_contains", ""));
    }
    if (body.contains("max_output_bytes")) {
      spec.set_max_output_bytes(body.value("max_output_bytes", 0ULL));
    }
    if (body.contains("level_in")) {
      for (const auto& level : body["level_in"]) {
        spec.add_level_in(level.get<std::string>());
      }
    }
    request.spec = spec;
  } else if (type == "pcap") {
    ops::v1::PcapSpec spec;
    spec.set_if_name(body.value("if_name", ""));
    if (body.contains("bpf")) {
      spec.set_bpf(body.value("bpf", ""));
    }
    spec.set_packet_limit(body.value("packet_limit", 0ULL));
    spec.set_snaplen(body.value("snaplen", 0U));
    spec.set_payload_trim_bytes(body.value("payload_trim_bytes", 0U));
    spec.set_promisc(body.value("promisc", false));
    request.spec = spec;
  } else if (type == "exec") {
    ops::v1::ExecSpec spec;
    spec.set_cmd(body.value("cmd", ""));
    if (body.contains("args")) {
      for (const auto& arg : body["args"]) {
        spec.add_args(arg.get<std::string>());
      }
    }
    spec.set_max_output_bytes(body.value("max_output_bytes", 0ULL));
    request.spec = spec;
  } else {
    throw std::invalid_argument("unsupported task type");
  }
  return request;
}

}  // namespace

HttpServer::HttpServer(HttpServerOptions options,
                       std::shared_ptr<runtime::TaskQueryService> query_service,
                       std::shared_ptr<runtime::TaskScheduler> scheduler)
    : options_(std::move(options)),
      query_service_(std::move(query_service)),
      scheduler_(std::move(scheduler)) {
  RegisterRoutes();
}

HttpServer::~HttpServer() { Stop(); }

void HttpServer::Start() {
  if (running_) {
    return;
  }
  running_ = true;
  thread_ = std::thread([this] {
    server_.listen(options_.listen_address, options_.port);
    running_ = false;
  });
}

void HttpServer::Stop() {
  if (!running_) {
    if (thread_.joinable()) {
      thread_.join();
    }
    return;
  }
  server_.stop();
  if (thread_.joinable()) {
    thread_.join();
  }
  running_ = false;
}

void HttpServer::RegisterRoutes() {
  server_.Get("/health", [](const httplib::Request&, httplib::Response& res) {
    WriteJson(res, 200, json{{"status", "ok"}});
  });

  server_.Get("/process/self", [](const httplib::Request&, httplib::Response& res) {
    WriteJson(res, 200, system::AgentProcessInfoToJson(system::GetAgentProcessInfo()));
  });

  server_.Get("/processes", [](const httplib::Request&, httplib::Response& res) {
    WriteJson(res, 200, json{{"processes", system::ProcessListToJson(system::ListProcesses())}});
  });

  server_.Get("/tasks", [this](const httplib::Request&, httplib::Response& res) {
    json items = json::array();
    if (query_service_) {
      for (const auto& task : query_service_->ListTasks()) {
        items.push_back(TaskToJson(task));
      }
    }
    WriteJson(res, 200, json{{"tasks", std::move(items)}});
  });

  server_.Post("/tasks", [this](const httplib::Request& req, httplib::Response& res) {
    if (!scheduler_) {
      WriteJson(res, 503, json{{"error", "task scheduler unavailable"}});
      return;
    }
    try {
      const auto body = json::parse(req.body);
      auto request = ParseTaskRequest(body);
      auto result = scheduler_->Submit(request);
      const int status = result.accepted ? 202 : 409;
      WriteJson(res, status,
                json{{"accepted", result.accepted},
                     {"op_id", result.op_id},
                     {"task_kind", runtime::ToString(result.kind)},
                     {"reason", result.reason}});
    } catch (const std::exception& ex) {
      WriteJson(res, 400, json{{"error", ex.what()}});
    }
  });

  server_.Get(R"(/tasks/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
    const auto op_id = static_cast<std::uint32_t>(std::stoul(req.matches[1].str()));
    if (!query_service_) {
      WriteJson(res, 503, json{{"error", "task query service unavailable"}});
      return;
    }
    auto task = query_service_->GetTask(op_id);
    if (!task) {
      WriteJson(res, 404, json{{"error", "task not found"}});
      return;
    }
    WriteJson(res, 200, TaskToJson(*task));
  });

  server_.Get(R"(/tasks/(\d+)/events)",
              [this](const httplib::Request& req, httplib::Response& res) {
                const auto op_id = static_cast<std::uint32_t>(std::stoul(req.matches[1].str()));
                if (!query_service_) {
                  WriteJson(res, 503, json{{"error", "task query service unavailable"}});
                  return;
                }
                json items = json::array();
                for (const auto& event : query_service_->ListEvents(op_id)) {
                  items.push_back(EventToJson(event));
                }
                WriteJson(res, 200, json{{"events", std::move(items)}});
              });

  server_.Delete(R"(/tasks/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
    if (!scheduler_) {
      WriteJson(res, 503, json{{"error", "task scheduler unavailable"}});
      return;
    }
    const auto op_id = static_cast<std::uint32_t>(std::stoul(req.matches[1].str()));
    const bool cancelled = scheduler_->Cancel(op_id);
    if (!cancelled) {
      WriteJson(res, 404, json{{"error", "task not found"}});
      return;
    }
    WriteJson(res, 202, json{{"cancelled", true}, {"op_id", op_id}});
  });
}

}  // namespace zurg::agent::http
