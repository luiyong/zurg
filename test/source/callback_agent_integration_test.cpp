#include "agent/agent_impl.h"

#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

#include "os.grpc.pb.h"
#include "zurg/logger_manager.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <utility>
#include <sstream>

#include <spdlog/sinks/ringbuffer_sink.h>

namespace {

using namespace std::chrono_literals;

class MockControlService : public ops::v1::Control::CallbackService {
 public:
  MockControlService() = default;

  bool WaitForStream(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mu_);
    return cv_.wait_for(lock, timeout, [&] { return reactor_ != nullptr; });
  }

  bool WaitForMessages(std::size_t count, std::chrono::milliseconds timeout) {
    std::shared_ptr<StreamData> data = SharedData();
    if (!data) return false;
    std::unique_lock<std::mutex> lock(data->mu);
    return data->cv.wait_for(lock, timeout,
                             [&] { return data->messages.size() >= count; });
  }

  std::vector<ops::v1::AgentToServer> SnapshotMessages() const {
    std::shared_ptr<StreamData> data = SharedData();
    if (!data) return {};
    std::lock_guard<std::mutex> lock(data->mu);
    return data->messages;
  }

  bool SendStartOp(const std::string& op_id, const ops::v1::PcapSpec& spec) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!reactor_) return false;
    ops::v1::ServerToAgent msg;
    auto* start = msg.mutable_start();
    start->mutable_meta()->set_op_id(op_id);
    start->mutable_pcap()->CopyFrom(spec);
    reactor_->EnqueueMessage(std::move(msg));
    return true;
  }

  bool SendShutdown(bool drain) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!reactor_) return false;
    ops::v1::ServerToAgent msg;
    msg.mutable_shutdown()->set_drain(drain);
    reactor_->EnqueueMessage(std::move(msg), !drain);
    return true;
  }

  bool WaitForDone(std::chrono::milliseconds timeout) {
    std::shared_ptr<StreamData> data = SharedData();
    if (!data) return false;
    std::unique_lock<std::mutex> lock(data->mu);
    return data->cv.wait_for(lock, timeout, [&] { return data->done; });
  }

 private:
  struct StreamData {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<ops::v1::AgentToServer> messages;
    std::deque<ops::v1::ServerToAgent> send_queue;
    bool write_in_flight = false;
    bool finish_requested = false;
    bool finish_started = false;
    bool done = false;
    grpc::Status status = grpc::Status::OK;
  };

  class Reactor : public grpc::ServerBidiReactor<ops::v1::AgentToServer, ops::v1::ServerToAgent> {
   public:
    Reactor(MockControlService* parent, std::shared_ptr<StreamData> data)
        : parent_(parent), data_(std::move(data)) {
      this->StartRead(&incoming_);
    }

    void EnqueueMessage(ops::v1::ServerToAgent msg, bool finish = false) {
      std::unique_lock<std::mutex> lock(data_->mu);
      data_->send_queue.push_back(std::move(msg));
      if (finish) data_->finish_requested = true;
      MaybeStartWriteLocked(lock);
    }

    void OnReadDone(bool ok) override {
      if (!ok) {
        std::unique_lock<std::mutex> lock(data_->mu);
        data_->finish_requested = true;
        MaybeFinishLocked(std::move(lock));
        return;
      }
      {
        std::lock_guard<std::mutex> lock(data_->mu);
        data_->messages.push_back(incoming_);
        data_->cv.notify_all();
      }
      this->StartRead(&incoming_);
    }

    void OnWriteDone(bool ok) override {
      std::unique_lock<std::mutex> lock(data_->mu);
      data_->write_in_flight = false;
      if (!ok) {
        data_->send_queue.clear();
        data_->finish_requested = false;
        lock.unlock();
        Finish(grpc::Status::CANCELLED);
        return;
      }
      MaybeStartWriteLocked(lock);
      MaybeFinishLocked(std::move(lock));
      return;
    }

    void OnDone() override {
      {
        std::lock_guard<std::mutex> lock(data_->mu);
        data_->done = true;
        data_->cv.notify_all();
      }
      parent_->OnReactorDone(this);
      delete this;
    }

   private:
    void MaybeStartWriteLocked(std::unique_lock<std::mutex>& lock) {
      if (data_->write_in_flight || data_->send_queue.empty()) return;
      outgoing_ = std::move(data_->send_queue.front());
      data_->send_queue.pop_front();
      data_->write_in_flight = true;
      auto* message = &outgoing_;
      lock.unlock();
      this->StartWrite(message);
      lock.lock();
    }

    void MaybeFinishLocked(std::unique_lock<std::mutex> lock) {
      if (data_->finish_started || !data_->finish_requested) return;
      if (data_->write_in_flight || !data_->send_queue.empty()) return;
      data_->finish_started = true;
      lock.unlock();
      Finish(grpc::Status::OK);
    }

    MockControlService* parent_;
    std::shared_ptr<StreamData> data_;
    ops::v1::AgentToServer incoming_;
    ops::v1::ServerToAgent outgoing_;
  };

  std::shared_ptr<StreamData> SharedData() const {
    std::lock_guard<std::mutex> lock(mu_);
    return data_;
  }

  void OnReactorDone(Reactor* reactor) {
    std::lock_guard<std::mutex> lock(mu_);
    if (reactor_ == reactor) {
      reactor_ = nullptr;
    }
  }

  ::grpc::ServerBidiReactor<ops::v1::AgentToServer, ops::v1::ServerToAgent>* Connect(
      ::grpc::CallbackServerContext* /*context*/) override {
    auto data = std::make_shared<StreamData>();
    auto* reactor = new Reactor(this, data);
    {
      std::lock_guard<std::mutex> lock(mu_);
      reactor_ = reactor;
      data_ = std::move(data);
    }
    cv_.notify_all();
    return reactor;
  }

  mutable std::mutex mu_;
  mutable std::condition_variable cv_;
  Reactor* reactor_ = nullptr;
  std::shared_ptr<StreamData> data_;
};

class CallbackAgentIntegrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    zurg::agent::internal::ResetForTests();
    zurg::agent::internal::SetBackoffHookForTests([](std::size_t) { return 1ms; });
    zurg::agent::internal::SetSleepHookForTests([](std::chrono::milliseconds) {});

    sink_ = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(128);
    zurg::agent::internal::SetLoggerSinkForTests(sink_);

    grpc::ServerBuilder builder;
    builder.RegisterService(&service_);
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port_);
    server_ = builder.BuildAndStart();
    ASSERT_TRUE(server_);
    ASSERT_GT(port_, 0);

    auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(port_), grpc::InsecureChannelCredentials());
    stub_ = ops::v1::Control::NewStub(channel);
  }

  void TearDown() override {
    zurg::agent::internal::RequestStop();
    if (agent_thread_.joinable()) agent_thread_.join();
    if (server_) {
      server_->Shutdown();
      server_->Wait();
    }
    zurg::agent::internal::SetLoggerSinkForTests(nullptr);
    zurg::agent::internal::SetSendHookForTests(nullptr);
    zurg::logging::LoggerManager::shutdown();
  }

  void StartAgentThread(const std::string& agent_id = "agent-test") {
    agent_thread_ = std::thread([&, agent_id] { zurg::agent::StartAgent(stub_.get(), agent_id); });
  }

  MockControlService service_;
  std::unique_ptr<grpc::Server> server_;
  std::unique_ptr<ops::v1::Control::Stub> stub_;
  int port_ = 0;
  std::thread agent_thread_;
  std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> sink_;
};

TEST_F(CallbackAgentIntegrationTest, HandlesPcapStartAndShutdown) {
  std::vector<ops::v1::AgentToServer> sent;
  std::mutex sent_mu;
  zurg::agent::internal::SetSendHookForTests([&](const ops::v1::AgentToServer& msg) {
    std::lock_guard<std::mutex> lock(sent_mu);
    sent.push_back(msg);
  });

  StartAgentThread();
  ASSERT_TRUE(service_.WaitForStream(3000ms));

  ASSERT_TRUE(service_.WaitForMessages(1, 1000ms));
  auto messages = service_.SnapshotMessages();
  ASSERT_GE(messages.size(), 1u);
  EXPECT_EQ(messages[0].msg_case(), ops::v1::AgentToServer::kHello);

  ops::v1::PcapSpec spec;
  spec.set_packet_limit(2);
  spec.set_payload_trim_bytes(4);
  ASSERT_TRUE(service_.SendStartOp("op1", spec));

  ASSERT_TRUE(service_.WaitForMessages(5, 2000ms));
  messages = service_.SnapshotMessages();
  ASSERT_GE(messages.size(), 5u);
  EXPECT_EQ(messages[1].msg_case(), ops::v1::AgentToServer::kAck);
  EXPECT_TRUE(messages[1].ack().accepted());
  EXPECT_EQ(messages[1].ack().op_id(), "op1");

  bool saw_data = false;
  bool saw_eof = false;
  for (const auto& msg : messages) {
    if (msg.msg_case() == ops::v1::AgentToServer::kData && msg.data().has_pcap_packet()) {
      saw_data = true;
    }
    if (msg.msg_case() == ops::v1::AgentToServer::kEof && msg.eof().has_pcap()) {
      saw_eof = true;
    }
  }
  EXPECT_TRUE(saw_data);
  EXPECT_TRUE(saw_eof);

  ASSERT_TRUE(service_.SendShutdown(false));
  ASSERT_TRUE(service_.WaitForDone(2000ms));

  zurg::agent::internal::RequestStop();
  agent_thread_.join();

  {
    std::lock_guard<std::mutex> lock(sent_mu);
    ASSERT_FALSE(sent.empty());
    std::ostringstream oss;
    for (const auto& msg : sent) {
      oss << msg.msg_case() << ' ';
    }
    SCOPED_TRACE("captured msg cases: " + oss.str());
    EXPECT_EQ(sent.front().msg_case(), ops::v1::AgentToServer::kHello);
  }

  auto formatted_entries = sink_->last_formatted();
  EXPECT_FALSE(formatted_entries.empty());
  bool found_logger = false;
  for (const auto& line : formatted_entries) {
    if (line.find("agent.callback") != std::string::npos) {
      found_logger = true;
      break;
    }
  }
  EXPECT_TRUE(found_logger);
}

}  // namespace
