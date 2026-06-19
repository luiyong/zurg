#include "control/control_asio_client.h"

#include <utility>

namespace zurg::agent {
namespace {

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

}  // namespace

ControlAsioClient::ControlAsioClient(ops::v1::Control::StubInterface* stub, std::string agent_id,
                                     std::shared_ptr<runtime::TaskScheduler> scheduler,
                                     Options options,
                                     std::shared_ptr<spdlog::logger> logger)
    : stub_(stub),
      agent_id_(std::move(agent_id)),
      scheduler_(std::move(scheduler)),
      options_(std::move(options)),
      logger_(std::move(logger)) {}

ControlAsioClient::~ControlAsioClient() { Stop(); }

void ControlAsioClient::Run() {
  running_.store(true);
  std::size_t attempt = 0;
  while (ShouldContinue()) {
    RunOnce();
    if (!ShouldContinue()) {
      break;
    }
    ++attempt;
    auto delay = options_.backoff_fn ? options_.backoff_fn(attempt)
                                     : std::chrono::milliseconds{0};
    if (logger_) {
      logger_->warn("asio control stream closed, reconnecting in {} ms", delay.count());
    }
    if (options_.sleep_fn) {
      options_.sleep_fn(delay);
    }
  }
  running_.store(false);
}

void ControlAsioClient::Stop() { running_.store(false); }

bool ControlAsioClient::ShouldContinue() const {
  if (!running_.load()) {
    return false;
  }
  return !options_.should_run || options_.should_run();
}

void ControlAsioClient::RunOnce() {
  if (!stub_) {
    if (logger_) {
      logger_->error("missing gRPC stub for asio control client");
    }
    running_.store(false);
    return;
  }

  // TODO-C2 follow-up: replace this placeholder with asio-grpc bidirectional
  // streaming once the dependency is available in the build environment.
  (void)MakeHello(agent_id_);
  running_.store(false);
}

void ControlAsioClient::HandleMessage(const ops::v1::ServerToAgent& msg) {
  if (!scheduler_) {
    return;
  }
  switch (msg.msg_case()) {
    case ops::v1::ServerToAgent::kStart:
      scheduler_->Submit(msg.start(), runtime::TaskInputSource::kGrpc);
      break;
    case ops::v1::ServerToAgent::kCancel:
      scheduler_->Cancel(msg.cancel().op_id());
      break;
    case ops::v1::ServerToAgent::kShutdown:
      scheduler_->Shutdown(msg.shutdown().drain());
      break;
    case ops::v1::ServerToAgent::kPing:
    default:
      break;
  }
}

}  // namespace zurg::agent
