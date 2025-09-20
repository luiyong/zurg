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

using StreamInterface = grpc::ClientReaderWriterInterface<ops::v1::AgentToServer, ops::v1::ServerToAgent>;

struct StreamState {
  bool fail_first_write = false;
  bool first_write_attempted = false;
  grpc::Status finish_status = grpc::Status::OK;
  std::vector<ops::v1::AgentToServer> writes;
  std::vector<ops::v1::ServerToAgent> responses;
  std::size_t read_index = 0;
  bool finished = false;
  std::mutex mu;
  std::condition_variable cv;
};

class FakeStream final : public StreamInterface {
 public:
  explicit FakeStream(std::shared_ptr<StreamState> state) : state_(std::move(state)) {}

  void WaitForInitialMetadata() override {}

  bool Write(const ops::v1::AgentToServer& msg, ::grpc::WriteOptions /*options*/) override {
    std::lock_guard<std::mutex> lock(state_->mu);
    state_->writes.push_back(msg);
    bool result = true;
    if (state_->fail_first_write && !state_->first_write_attempted) {
      result = false;
    }
    state_->first_write_attempted = true;
    state_->cv.notify_all();
    return result;
  }

  bool WritesDone() override { return true; }

  bool NextMessageSize(uint32_t* sz) override {
    *sz = 0;
    return true;
  }

  bool Read(ops::v1::ServerToAgent* msg) override {
    std::lock_guard<std::mutex> lock(state_->mu);
    if (state_->read_index >= state_->responses.size()) {
      return false;
    }
    *msg = state_->responses[state_->read_index++];
    return true;
  }

  ::grpc::Status Finish() override {
    std::lock_guard<std::mutex> lock(state_->mu);
    state_->finished = true;
    state_->cv.notify_all();
    return state_->finish_status;
  }

 private:
  std::shared_ptr<StreamState> state_;
};

class FakeStub final : public ops::v1::Control::StubInterface {
 public:
  void AddScenario(std::shared_ptr<StreamState> state) {
    std::lock_guard<std::mutex> lock(mu_);
    scenarios_.push_back(std::move(state));
  }

  std::size_t connect_calls() const { return connect_calls_.load(); }

  bool WaitForConnections(std::size_t expected, std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(mu_);
    return cv_.wait_for(lock, timeout, [&] { return connect_calls_.load() >= expected; });
  }

  std::shared_ptr<StreamState> Scenario(std::size_t index) const {
    std::lock_guard<std::mutex> lock(mu_);
    return index < scenarios_.size() ? scenarios_[index] : nullptr;
  }

 private:
  ::grpc::ClientReaderWriterInterface<ops::v1::AgentToServer, ops::v1::ServerToAgent>* ConnectRaw(::grpc::ClientContext* /*context*/) override {
    std::shared_ptr<StreamState> state;
    {
      std::lock_guard<std::mutex> lock(mu_);
      const auto idx = connect_calls_.load();
      if (idx < scenarios_.size()) {
        state = scenarios_[idx];
      }
      connect_calls_.fetch_add(1);
    }
    cv_.notify_all();
    if (!state) {
      return nullptr;
    }
    return new FakeStream(std::move(state));
  }

  ::grpc::ClientAsyncReaderWriterInterface<ops::v1::AgentToServer, ops::v1::ServerToAgent>* AsyncConnectRaw(
      ::grpc::ClientContext* /*context*/, ::grpc::CompletionQueue* /*cq*/, void* /*tag*/) override {
    return nullptr;
  }

  ::grpc::ClientAsyncReaderWriterInterface<ops::v1::AgentToServer, ops::v1::ServerToAgent>* PrepareAsyncConnectRaw(
      ::grpc::ClientContext* /*context*/, ::grpc::CompletionQueue* /*cq*/) override {
    return nullptr;
  }

  mutable std::mutex mu_;
  mutable std::condition_variable cv_;
  std::vector<std::shared_ptr<StreamState>> scenarios_;
  mutable std::atomic<std::size_t> connect_calls_{0};
};

ops::v1::ServerToAgent MakeShutdownMessage() {
  ops::v1::ServerToAgent msg;
  msg.mutable_shutdown()->set_drain(false);
  return msg;
}

TEST(GnoiAgentTest, RetriesAfterInitialFailure) {
  using namespace std::chrono_literals;

  zurg::agent::internal::ResetForTests();
  zurg::agent::internal::SetBackoffHookForTests([](std::size_t) { return 1ms; });
  zurg::agent::internal::SetSleepHookForTests([](std::chrono::milliseconds) {});

  FakeStub stub;
  stub.AddScenario(nullptr);  // first attempt fails to create stream

  auto success_state = std::make_shared<StreamState>();
  success_state->responses.push_back(MakeShutdownMessage());
  success_state->finish_status = grpc::Status::OK;
  stub.AddScenario(success_state);

  std::thread agent_thread([&] { zurg::agent::StartAgent(&stub, "agent-test"); });

  ASSERT_TRUE(stub.WaitForConnections(2, 2s));

  {
    std::unique_lock<std::mutex> lock(success_state->mu);
    success_state->cv.wait_for(lock, 2s, [&] { return !success_state->writes.empty(); });
  }

  zurg::agent::internal::RequestStop();
  agent_thread.join();
  zurg::agent::internal::ResetForTests();

  EXPECT_GE(stub.connect_calls(), 2u);
  ASSERT_FALSE(success_state->writes.empty());
  const auto& hello = success_state->writes.front();
  ASSERT_EQ(hello.msg_case(), ops::v1::AgentToServer::kHello);
  EXPECT_EQ(hello.hello().agent_id(), "agent-test");
}

}  // namespace
