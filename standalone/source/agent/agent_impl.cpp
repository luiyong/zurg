#include "agent_impl.h"

#include <fmt/core.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <functional>
#include <mutex>
#include <thread>

namespace zurg::agent {

namespace internal {

namespace {
std::atomic<bool> g_running{true};
std::mutex g_hook_mu;
std::function<std::chrono::milliseconds(std::size_t)> g_backoff_hook;
std::function<void(std::chrono::milliseconds)> g_sleep_hook;
}

bool IsRunning() { return g_running.load(); }

void RequestStop() { g_running.store(false); }

void ClearTestHooks() {
  std::lock_guard<std::mutex> lock(g_hook_mu);
  g_backoff_hook = nullptr;
  g_sleep_hook = nullptr;
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

namespace {

using Stream = grpc::ClientReaderWriterInterface<ops::v1::AgentToServer, ops::v1::ServerToAgent>;

void HandleSignal(int signo) {
  if (signo == SIGINT || signo == SIGTERM) {
    internal::RequestStop();
  }
}

bool Send(Stream* stream, ops::v1::AgentToServer&& msg) {
  return stream && stream->Write(msg);
}

bool HandleServerMessage(Stream* stream, const ops::v1::ServerToAgent& msg, const std::string& agent_id) {
  using internal::MakePong;
  using internal::MakeRejectAck;
  switch (msg.msg_case()) {
    case ops::v1::ServerToAgent::kPing:
      return Send(stream, MakePong(agent_id, msg.ping()));
    case ops::v1::ServerToAgent::kStart:
      fmt::print("[agent] received StartOp for {}\n", msg.start().meta().op_id());
      return Send(stream, MakeRejectAck(msg.start()));
    case ops::v1::ServerToAgent::kCancel:
      fmt::print("[agent] received CancelOp for {}\n", msg.cancel().op_id());
      return true;
    case ops::v1::ServerToAgent::kShutdown:
      fmt::print("[agent] received shutdown request (drain={})\n", msg.shutdown().drain());
      internal::RequestStop();
      return false;
    case ops::v1::ServerToAgent::MSG_NOT_SET:
    default:
      fmt::print("[agent] received unknown ServerToAgent message\n");
      return true;
  }
}

}  // namespace

void StartAgent(ops::v1::Control::StubInterface* stub, const std::string& agent_id) {
  if (!stub) {
    fmt::print(stderr, "[agent] missing stub, aborting\n");
    return;
  }

  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);

  std::size_t attempt = 0;
  while (internal::IsRunning()) {
    grpc::ClientContext ctx;
    auto stream = stub->Connect(&ctx);
    if (!stream) {
      ++attempt;
      auto delay = internal::ComputeBackoff(attempt);
      fmt::print(stderr, "[agent] connect failed, retry in {} ms\n", delay.count());
      internal::SleepWithStop(delay);
      continue;
    }

    attempt = 0;
    if (!Send(stream.get(), internal::MakeHello(agent_id))) {
      fmt::print(stderr, "[agent] failed to send hello, will reconnect\n");
      ctx.TryCancel();
      stream->Finish();
      ++attempt;
      internal::SleepWithStop(internal::ComputeBackoff(attempt));
      continue;
    }

    ops::v1::ServerToAgent incoming;
    while (internal::IsRunning() && stream->Read(&incoming)) {
      if (!HandleServerMessage(stream.get(), incoming, agent_id)) {
        break;
      }
      incoming.Clear();
    }

    grpc::Status status = stream->Finish();
    if (!internal::IsRunning()) break;

    ++attempt;
    auto delay = internal::ComputeBackoff(attempt);
    fmt::print(stderr,
               "[agent] stream closed (code={}, message='{}'), reconnecting in {} ms\n",
               static_cast<int>(status.error_code()), status.error_message(), delay.count());
    internal::SleepWithStop(delay);
  }
}

}  // namespace zurg::agent
