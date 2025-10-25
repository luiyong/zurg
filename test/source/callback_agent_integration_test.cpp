#include "agent/agent_impl.h"

#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

#include "os.grpc.pb.h"
#include "zurg/logger_manager.h"
#include <google/protobuf/util/time_util.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
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

  bool SendPcapStart(std::uint32_t op_id, const ops::v1::PcapSpec& spec) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!reactor_) return false;
    ops::v1::ServerToAgent msg;
    auto* start = msg.mutable_start();
    start->mutable_meta()->set_op_id(op_id);
    start->mutable_pcap()->CopyFrom(spec);
    reactor_->EnqueueMessage(std::move(msg));
    return true;
  }

  bool SendLogFilterStart(std::uint32_t op_id, const ops::v1::LogFilterSpec& spec) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!reactor_) return false;
    ops::v1::ServerToAgent msg;
    auto* start = msg.mutable_start();
    start->mutable_meta()->set_op_id(op_id);
    start->mutable_log_filter()->CopyFrom(spec);
    reactor_->EnqueueMessage(std::move(msg));
    return true;
  }

  bool SendExecStart(std::uint32_t op_id, const ops::v1::ExecSpec& spec) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!reactor_) return false;
    ops::v1::ServerToAgent msg;
    auto* start = msg.mutable_start();
    start->mutable_meta()->set_op_id(op_id);
    start->mutable_exec()->CopyFrom(spec);
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

  bool SendCancel(std::uint32_t op_id) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!reactor_) return false;
    ops::v1::ServerToAgent msg;
    msg.mutable_cancel()->set_op_id(op_id);
    reactor_->EnqueueMessage(std::move(msg));
    return true;
  }

  bool SendPing(std::uint64_t seq) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!reactor_) return false;
    ops::v1::ServerToAgent msg;
    auto* ping = msg.mutable_ping();
    ping->set_agent_id("controller");
    ping->set_seq(seq);
    reactor_->EnqueueMessage(std::move(msg));
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

    void ForceFinish() {
      std::unique_lock<std::mutex> lock(data_->mu);
      data_->finish_requested = true;
      MaybeFinishLocked(std::move(lock));
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

 public:
  bool ForceCloseStream() {
    std::lock_guard<std::mutex> lock(mu_);
    if (!reactor_) return false;
    reactor_->ForceFinish();
    return true;
  }

 private:
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
    zurg::agent::internal::SetAdditionalLoggerSink(sink_);

    namespace fs = std::filesystem;
    auto base_tmp = fs::temp_directory_path();
    log_dir_ = base_tmp / fs::path("zurg-logfilter-test") /
               fs::path(std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(log_dir_);
    zurg::log_ops::Options log_opts;
    log_opts.log_root = log_dir_.string();
    log_opts.temp_dir = log_dir_.string();
    log_opts.base_path = "agent.log";
    log_opts.include_rotations = true;
    log_opts.rotation_scan_depth = 2;
    log_opts.output_basename = "filtered";
    zurg::agent::internal::SetLogOptions(std::move(log_opts));

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
    zurg::agent::internal::SetAdditionalLoggerSink(nullptr);
    zurg::agent::internal::SetSendHookForTests(nullptr);
    zurg::logging::LoggerManager::shutdown();
    if (!log_dir_.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(log_dir_, ec);
    }
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
  std::filesystem::path log_dir_;
};

TEST_F(CallbackAgentIntegrationTest, HandlesLogFilterAndShutdown) {
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

  namespace fs = std::filesystem;
  fs::path base_log = log_dir_ / "agent.log";
  {
    std::ofstream os(base_log);
    ASSERT_TRUE(os.is_open());
    os << "[2025-09-27 11:00:00.000] [agent.callback] [info] keep-1" << std::endl;
    os << "[2025-09-27 11:05:00.000] [agent.callback] [error] drop-1" << std::endl;
  }
  fs::path rotated_log = log_dir_ / "agent.log.1";
  {
    std::ofstream os(rotated_log);
    ASSERT_TRUE(os.is_open());
    os << "[2025-09-27 10:50:00.000] [agent.callback] [info] old-line" << std::endl;
  }

  ops::v1::LogFilterSpec spec;
  spec.add_level_in("info");
  spec.set_grep_contains("keep");
  google::protobuf::Timestamp start_time;
  google::protobuf::Timestamp end_time;
  ASSERT_TRUE(google::protobuf::util::TimeUtil::FromString("2025-09-27T10:55:00Z", &start_time));
  ASSERT_TRUE(google::protobuf::util::TimeUtil::FromString("2025-09-27T11:10:00Z", &end_time));
  *spec.mutable_start_time() = start_time;
  *spec.mutable_end_time() = end_time;

  ASSERT_TRUE(service_.SendLogFilterStart(1, spec));

  ASSERT_TRUE(service_.WaitForMessages(4, 2000ms));
  messages = service_.SnapshotMessages();
  ASSERT_GE(messages.size(), 4u);
  EXPECT_EQ(messages[1].msg_case(), ops::v1::AgentToServer::kAck);
  EXPECT_TRUE(messages[1].ack().accepted());
  EXPECT_EQ(messages[1].ack().op_id(), 1u);

  bool saw_data = false;
  bool saw_eof = false;
  std::string concatenated;
  for (const auto& msg : messages) {
    if (msg.msg_case() == ops::v1::AgentToServer::kData && msg.data().has_log_chunk()) {
      saw_data = true;
      concatenated.append(msg.data().log_chunk().data());
    }
    if (msg.msg_case() == ops::v1::AgentToServer::kEof && msg.eof().has_log()) {
      saw_eof = true;
      EXPECT_EQ(msg.eof().log().total_lines(), 1);
      EXPECT_GE(msg.eof().log().total_size(), 0);
    }
  }
  EXPECT_TRUE(saw_data);
  EXPECT_TRUE(saw_eof);
  EXPECT_NE(concatenated.find("keep-1"), std::string::npos);
  EXPECT_EQ(concatenated.find("old-line"), std::string::npos);

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

TEST_F(CallbackAgentIntegrationTest, CancelsLogTask) {
  std::vector<ops::v1::AgentToServer> sent;
  std::mutex sent_mu;
  zurg::agent::internal::SetSendHookForTests([&](const ops::v1::AgentToServer& msg) {
    std::lock_guard<std::mutex> lock(sent_mu);
    sent.push_back(msg);
  });

  StartAgentThread();
  ASSERT_TRUE(service_.WaitForStream(3000ms));
  ASSERT_TRUE(service_.WaitForMessages(1, 1000ms));

  namespace fs = std::filesystem;
  fs::path log_file = log_dir_ / "cancel.log";
  {
    std::ofstream os(log_file);
    ASSERT_TRUE(os.is_open());
    for (int i = 0; i < 2000; ++i) {
      os << "[2025-09-27 11:00:00.000] [agent.test] [info] keep " << i << std::endl;
    }
  }

  ops::v1::LogFilterSpec spec;
  spec.add_level_in("info");
  google::protobuf::Timestamp start_ts;
  google::protobuf::Timestamp end_ts;
  ASSERT_TRUE(google::protobuf::util::TimeUtil::FromString("2025-09-27T10:00:00Z", &start_ts));
  ASSERT_TRUE(google::protobuf::util::TimeUtil::FromString("2025-09-27T12:00:00Z", &end_ts));
  *spec.mutable_start_time() = start_ts;
  *spec.mutable_end_time() = end_ts;

  ASSERT_TRUE(service_.SendLogFilterStart(1, spec));
  ASSERT_TRUE(service_.WaitForMessages(2, 1000ms));
  std::this_thread::sleep_for(50ms);
  ASSERT_TRUE(service_.SendCancel(1));
  ASSERT_TRUE(service_.WaitForMessages(3, 1000ms));

  bool saw_error = false;
  bool saw_eof = false;
  {
    std::lock_guard<std::mutex> lock(sent_mu);
    for (const auto& msg : sent) {
    if (msg.msg_case() == ops::v1::AgentToServer::kError && msg.error().op_id() == 1u) {
        EXPECT_TRUE(msg.error().code() == "CANCELLED" ||
                    msg.error().code() == std::to_string(static_cast<int>(::grpc::StatusCode::CANCELLED)));
        saw_error = true;
      }
    if (msg.msg_case() == ops::v1::AgentToServer::kEof && msg.eof().op_id() == 1u) {
        saw_eof = true;
      }
    }
  }
  EXPECT_TRUE(saw_error || saw_eof);

  ASSERT_TRUE(service_.SendShutdown(false));
  ASSERT_TRUE(service_.WaitForDone(2000ms));

  zurg::agent::internal::RequestStop();
  agent_thread_.join();
}

TEST_F(CallbackAgentIntegrationTest, SequentialLogTasksExecuteInOrder) {
  std::vector<std::pair<int, std::uint32_t>> events;
  std::mutex events_mu;
  zurg::agent::internal::SetSendHookForTests([&](const ops::v1::AgentToServer& msg) {
    std::lock_guard<std::mutex> lock(events_mu);
    if (msg.msg_case() == ops::v1::AgentToServer::kData) {
        events.emplace_back(0, msg.data().op_id());
    } else if (msg.msg_case() == ops::v1::AgentToServer::kEof) {
        events.emplace_back(1, msg.eof().op_id());
    }
  });

  StartAgentThread();
  ASSERT_TRUE(service_.WaitForStream(3000ms));
  ASSERT_TRUE(service_.WaitForMessages(1, 1000ms));

  namespace fs = std::filesystem;
  fs::path log1 = log_dir_ / "seq1.log";
  fs::path log2 = log_dir_ / "seq2.log";
  {
    std::ofstream os(log1);
    ASSERT_TRUE(os.is_open());
    os << "[2025-09-27 11:00:00.000] [agent.test] [info] first" << std::endl;
  }
  {
    std::ofstream os(log2);
    ASSERT_TRUE(os.is_open());
    os << "[2025-09-27 11:10:00.000] [agent.test] [info] second" << std::endl;
  }

  auto make_spec = [](const std::string& path, const std::string& start, const std::string& end) {
    ops::v1::LogFilterSpec spec;
    // base paths provided via log options
    spec.add_level_in("info");
    google::protobuf::Timestamp s;
    google::protobuf::Timestamp e;
    EXPECT_TRUE(google::protobuf::util::TimeUtil::FromString(start, &s));
    EXPECT_TRUE(google::protobuf::util::TimeUtil::FromString(end, &e));
    *spec.mutable_start_time() = s;
    *spec.mutable_end_time() = e;
    return spec;
  };

  auto spec1 = make_spec(log1.string(), "2025-09-27T10:55:00Z", "2025-09-27T11:05:00Z");
  auto spec2 = make_spec(log2.string(), "2025-09-27T11:05:00Z", "2025-09-27T11:20:00Z");

  ASSERT_TRUE(service_.SendLogFilterStart(1, spec1));
  ASSERT_TRUE(service_.SendLogFilterStart(2, spec2));

  ASSERT_TRUE(service_.WaitForMessages(6, 2000ms));

  std::vector<std::pair<int, std::uint32_t>> snapshot;
  {
    std::lock_guard<std::mutex> lock(events_mu);
    snapshot = events;
  }

  auto first_op2_data = std::find_if(snapshot.begin(), snapshot.end(), [](const auto& ev) {
    return ev.first == 0 && ev.second == 2u;
  });
  auto op1_eof = std::find_if(snapshot.begin(), snapshot.end(), [](const auto& ev) {
    return ev.first == 1 && ev.second == 1u;
  });
  ASSERT_NE(op1_eof, snapshot.end());
  if (first_op2_data != snapshot.end()) {
    EXPECT_TRUE(op1_eof < first_op2_data);
  }

  ASSERT_TRUE(service_.SendShutdown(false));
  ASSERT_TRUE(service_.WaitForDone(2000ms));

  zurg::agent::internal::RequestStop();
  agent_thread_.join();
}

TEST_F(CallbackAgentIntegrationTest, HandlesExecTaskCollectsInterfaces) {
  std::vector<ops::v1::AgentToServer> sent;
  std::mutex sent_mu;
  zurg::agent::internal::SetSendHookForTests([&](const ops::v1::AgentToServer& msg) {
    std::lock_guard<std::mutex> lock(sent_mu);
    sent.push_back(msg);
  });

  StartAgentThread();
  ASSERT_TRUE(service_.WaitForStream(3000ms));
  ASSERT_TRUE(service_.WaitForMessages(1, 1000ms));

  ops::v1::ExecSpec spec;
  spec.set_cmd("ip");
  spec.add_args("addr");
  ASSERT_TRUE(service_.SendExecStart(1, spec));
  ASSERT_TRUE(service_.WaitForMessages(4, 2000ms));

  auto messages = service_.SnapshotMessages();
  ASSERT_GE(messages.size(), 4u);
  EXPECT_EQ(messages[1].msg_case(), ops::v1::AgentToServer::kAck);
  EXPECT_TRUE(messages[1].ack().accepted());

  bool saw_data = false;
  bool saw_eof = false;
  for (const auto& msg : messages) {
    if (msg.msg_case() == ops::v1::AgentToServer::kData && msg.data().has_exec_chunk()) {
      saw_data = true;
      EXPECT_NE(msg.data().exec_chunk().stdout().find("interface"), std::string::npos);
    }
    if (msg.msg_case() == ops::v1::AgentToServer::kEof && msg.eof().has_exec()) {
      saw_eof = true;
      EXPECT_EQ(msg.eof().exec().code(), 0);
    }
  }
  EXPECT_TRUE(saw_data);
  EXPECT_TRUE(saw_eof);

  ASSERT_TRUE(service_.SendShutdown(false));
  ASSERT_TRUE(service_.WaitForDone(2000ms));

  zurg::agent::internal::RequestStop();
  agent_thread_.join();
}

TEST_F(CallbackAgentIntegrationTest, ReconnectSendsHello) {
  using namespace std::chrono_literals;

  StartAgentThread();
  ASSERT_TRUE(service_.WaitForStream(3000ms));
  ASSERT_TRUE(service_.WaitForMessages(1, 1000ms));

  auto first_batch = service_.SnapshotMessages();
  ASSERT_FALSE(first_batch.empty());
  EXPECT_EQ(first_batch.front().msg_case(), ops::v1::AgentToServer::kHello);

  ASSERT_TRUE(service_.ForceCloseStream());
  ASSERT_TRUE(service_.WaitForStream(3000ms));
  ASSERT_TRUE(service_.WaitForMessages(1, 1000ms));

  auto second_batch = service_.SnapshotMessages();
  ASSERT_FALSE(second_batch.empty());
  EXPECT_EQ(second_batch.front().msg_case(), ops::v1::AgentToServer::kHello);

  zurg::agent::internal::RequestStop();
  service_.ForceCloseStream();
  if (agent_thread_.joinable()) {
    agent_thread_.join();
  }
}

TEST_F(CallbackAgentIntegrationTest, RespondsToPingWithPong) {
  using namespace std::chrono_literals;

  StartAgentThread();
  ASSERT_TRUE(service_.WaitForStream(3000ms));
  ASSERT_TRUE(service_.WaitForMessages(1, 1000ms));

  ASSERT_TRUE(service_.SendPing(42));
  ASSERT_TRUE(service_.WaitForMessages(2, 1000ms));

  auto messages = service_.SnapshotMessages();
  ASSERT_GE(messages.size(), 2u);
  const auto& pong = messages.back();
  EXPECT_EQ(pong.msg_case(), ops::v1::AgentToServer::kPong);
  EXPECT_EQ(pong.pong().seq(), 42u);

  zurg::agent::internal::RequestStop();
  service_.ForceCloseStream();
  if (agent_thread_.joinable()) {
    agent_thread_.join();
  }
}

TEST_F(CallbackAgentIntegrationTest, HandlesSyntheticPcapTask) {
  using namespace std::chrono_literals;

  StartAgentThread();
  ASSERT_TRUE(service_.WaitForStream(3000ms));
  ASSERT_TRUE(service_.WaitForMessages(1, 1000ms));

  ops::v1::PcapSpec spec;
  spec.set_packet_limit(3);
  spec.set_snaplen(64);
  spec.set_payload_trim_bytes(32);

  ASSERT_TRUE(service_.SendPcapStart(101, spec));
  ASSERT_TRUE(service_.WaitForMessages(6, 3000ms));

  auto messages = service_.SnapshotMessages();
  bool saw_ack = false;
  int data_count = 0;
  bool saw_eof = false;
  for (const auto& msg : messages) {
    if (msg.msg_case() == ops::v1::AgentToServer::kAck && msg.ack().op_id() == 101) {
      if (msg.ack().accepted()) {
        saw_ack = true;
      }
    }
    if (msg.msg_case() == ops::v1::AgentToServer::kData && msg.data().has_log_chunk() &&
        msg.data().op_id() == 101) {
      ++data_count;
    }
    if (msg.msg_case() == ops::v1::AgentToServer::kEof && msg.eof().has_pcap() &&
        msg.eof().op_id() == 101) {
      saw_eof = true;
    }
  }

  EXPECT_TRUE(saw_ack);
  EXPECT_GE(data_count, 1);
  EXPECT_TRUE(saw_eof);

  ASSERT_TRUE(service_.SendShutdown(false));
  ASSERT_TRUE(service_.WaitForDone(2000ms));

  zurg::agent::internal::RequestStop();
  agent_thread_.join();
}

TEST_F(CallbackAgentIntegrationTest, ShutdownDrainCompletesPendingTasks) {
  using namespace std::chrono_literals;

  StartAgentThread();
  ASSERT_TRUE(service_.WaitForStream(3000ms));
  ASSERT_TRUE(service_.WaitForMessages(1, 1000ms));

  namespace fs = std::filesystem;
  fs::path base_log = log_dir_ / "agent.log";
  {
    std::ofstream os(base_log);
    ASSERT_TRUE(os.is_open());
    os << "[2025-09-27 11:00:00.000] [agent.callback] [info] keep-drain" << std::endl;
  }

  ops::v1::LogFilterSpec spec;
  spec.add_level_in("info");
  spec.set_grep_contains("keep");
  google::protobuf::Timestamp start_time;
  google::protobuf::Timestamp end_time;
  ASSERT_TRUE(google::protobuf::util::TimeUtil::FromString("2025-09-27T10:55:00Z", &start_time));
  ASSERT_TRUE(google::protobuf::util::TimeUtil::FromString("2025-09-27T11:05:00Z", &end_time));
  *spec.mutable_start_time() = start_time;
  *spec.mutable_end_time() = end_time;

  ASSERT_TRUE(service_.SendLogFilterStart(201, spec));
  ASSERT_TRUE(service_.WaitForMessages(2, 2000ms));

  ASSERT_TRUE(service_.SendShutdown(true));
  ASSERT_TRUE(service_.WaitForMessages(4, 2000ms));

  auto messages = service_.SnapshotMessages();
  std::size_t ack_index = std::numeric_limits<std::size_t>::max();
  std::size_t eof_index = std::numeric_limits<std::size_t>::max();
  for (std::size_t i = 0; i < messages.size(); ++i) {
    const auto& msg = messages[i];
    if (msg.msg_case() == ops::v1::AgentToServer::kAck && msg.ack().op_id() == 201 &&
        msg.ack().accepted()) {
      ack_index = std::min(ack_index, i);
    }
    if (msg.msg_case() == ops::v1::AgentToServer::kEof && msg.eof().has_log() &&
        msg.eof().op_id() == 201) {
      eof_index = std::min(eof_index, i);
    }
  }

  EXPECT_NE(ack_index, std::numeric_limits<std::size_t>::max());
  EXPECT_NE(eof_index, std::numeric_limits<std::size_t>::max());
  EXPECT_LT(ack_index, eof_index);

  ASSERT_TRUE(service_.ForceCloseStream());
  ASSERT_TRUE(service_.WaitForDone(2000ms));

  zurg::agent::internal::RequestStop();
  if (agent_thread_.joinable()) {
    agent_thread_.join();
  }
}

}  // namespace
