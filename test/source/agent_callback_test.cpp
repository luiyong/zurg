#include "agent/agent_impl.h"

#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

#include "os.grpc.pb.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

class FakeStub final : public ops::v1::Control::StubInterface {
 public:
  struct Script {
    ::grpc::Status status;
  };

  FakeStub() : async_(this) {}

  void AddScript(Script script) {
    std::lock_guard<std::mutex> lock(mu_);
    scripts_.push_back(std::move(script));
  }

  std::size_t connect_calls() const { return connect_calls_.load(); }

  bool WaitForConnections(std::size_t expected, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mu_);
    return cv_.wait_for(lock, timeout, [&] { return connect_calls_.load() >= expected; });
  }

  void Complete(std::size_t index) {
    std::shared_ptr<ActiveCall> call;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (index >= active_calls_.size()) {
        return;
      }
      call = active_calls_[index];
    }
    if (!call) return;
    call->reactor->OnWriteDone(true);
    call->reactor->OnDone(call->script.status);
  }

 private:
  ::grpc::ClientReaderWriterInterface<ops::v1::AgentToServer, ops::v1::ServerToAgent>* ConnectRaw(::grpc::ClientContext*) override {
    return nullptr;
  }

  ::grpc::ClientAsyncReaderWriterInterface<ops::v1::AgentToServer, ops::v1::ServerToAgent>* AsyncConnectRaw(
      ::grpc::ClientContext*, ::grpc::CompletionQueue*, void*) override {
    return nullptr;
  }

  ::grpc::ClientAsyncReaderWriterInterface<ops::v1::AgentToServer, ops::v1::ServerToAgent>* PrepareAsyncConnectRaw(
      ::grpc::ClientContext*, ::grpc::CompletionQueue*) override {
    return nullptr;
  }

  class Async final : public ops::v1::Control::StubInterface::experimental_async_interface {
   public:
    explicit Async(FakeStub* parent) : parent_(parent) {}

    void Connect(::grpc::ClientContext* context,
                 ::grpc::ClientBidiReactor<ops::v1::AgentToServer, ops::v1::ServerToAgent>* reactor) override {
      parent_->HandleConnect(context, reactor);
    }

   private:
    FakeStub* parent_;
  };

  experimental_async_interface* experimental_async() override { return &async_; }

  void HandleConnect(::grpc::ClientContext*,
                     ::grpc::ClientBidiReactor<ops::v1::AgentToServer, ops::v1::ServerToAgent>* reactor) {
    Script script;
    std::shared_ptr<ActiveCall> call = std::make_shared<ActiveCall>();
    call->reactor = reactor;
    {
      std::lock_guard<std::mutex> lock(mu_);
      const std::size_t idx = connect_calls_.load();
      ++connect_calls_;
      cv_.notify_all();
      if (idx < scripts_.size()) {
        script = scripts_[idx];
      } else {
        script.status = grpc::Status::OK;
      }
      call->script = script;
      if (idx >= active_calls_.size()) {
        active_calls_.push_back(call);
      } else {
        active_calls_[idx] = call;
      }
    }
  }

  mutable std::mutex mu_;
  mutable std::condition_variable cv_;
  std::vector<Script> scripts_;
  std::atomic<std::size_t> connect_calls_{0};
  struct ActiveCall {
    ::grpc::ClientBidiReactor<ops::v1::AgentToServer, ops::v1::ServerToAgent>* reactor;
    Script script;
  };
  std::vector<std::shared_ptr<ActiveCall>> active_calls_;
  Async async_{this};
};

ops::v1::AgentToServer::MsgCase FirstMessageCase(const std::vector<ops::v1::AgentToServer>& msgs) {
  if (msgs.empty()) return ops::v1::AgentToServer::MSG_NOT_SET;
  return msgs.front().msg_case();
}

TEST(CallbackAgentTest, DISABLED_RetriesAndSendsHello) {
  using namespace std::chrono_literals;

  zurg::agent::internal::ResetForTests();
  zurg::agent::internal::SetBackoffHookForTests([](std::size_t) { return std::chrono::milliseconds(1); });
  zurg::agent::internal::SetSleepHookForTests([](std::chrono::milliseconds) {});

  std::vector<ops::v1::AgentToServer> sent;
  std::mutex sent_mu;
  zurg::agent::internal::SetSendHookForTests([&](const ops::v1::AgentToServer& msg) {
    std::lock_guard<std::mutex> lock(sent_mu);
    sent.push_back(msg);
  });

  FakeStub stub;
  stub.AddScript({grpc::Status(grpc::StatusCode::UNAVAILABLE, "mock fail")});
  stub.AddScript({grpc::Status::OK});

  std::thread agent_thread([&] { zurg::agent::StartAgent(&stub, "agent-test"); });

  ASSERT_TRUE(stub.WaitForConnections(1, 1s));
  stub.Complete(0);
  ASSERT_TRUE(stub.WaitForConnections(2, 1s));
  stub.Complete(1);

  zurg::agent::internal::RequestStop();
  agent_thread.join();
  zurg::agent::internal::ResetForTests();

  std::lock_guard<std::mutex> lock(sent_mu);
  ASSERT_FALSE(sent.empty());
  EXPECT_EQ(FirstMessageCase(sent), ops::v1::AgentToServer::kHello);
  EXPECT_GE(stub.connect_calls(), 2u);
}

}  // namespace
