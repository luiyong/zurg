#ifndef ZURG_AGENT_CONTROL_CONTROL_STREAM_CLIENT_H_
#define ZURG_AGENT_CONTROL_CONTROL_STREAM_CLIENT_H_

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include <spdlog/logger.h>

#include "os.grpc.pb.h"

namespace zurg::agent {

class ControlStreamClient {
 public:
  struct Options {
    std::function<bool()> should_run;
    std::function<std::chrono::milliseconds(std::size_t)> backoff_fn;
    std::function<void(std::chrono::milliseconds)> sleep_fn;
    std::function<void(const ops::v1::AgentToServer&)> on_send;
  };

  using MessageCallback = std::function<void(const ops::v1::ServerToAgent&, bool)>;
  using ReadyCallback = std::function<void()>;
  using StreamClosedCallback = std::function<void(const grpc::Status&)>;

  ControlStreamClient(ops::v1::Control::StubInterface* stub, Options options,
                      std::shared_ptr<spdlog::logger> logger);
  ~ControlStreamClient();

  ControlStreamClient(const ControlStreamClient&) = delete;
  ControlStreamClient& operator=(const ControlStreamClient&) = delete;

  void SetMessageCallback(MessageCallback cb);
  void SetReadyCallback(ReadyCallback cb);
  void SetStreamClosedCallback(StreamClosedCallback cb);

  void Run();
  void Stop();
  void CancelStream();

  void EnqueueWrite(ops::v1::AgentToServer msg);

 private:
  class StreamReactor;

  bool ShouldContinue() const;
  void OnStreamReady(StreamReactor* reactor);
  void OnWriteFinished(bool ok);
  void OnMessage(const ops::v1::ServerToAgent& msg, bool ok);
  void OnStreamClosed(const grpc::Status& status);
  void MaybeStartWriteLocked();

  ops::v1::Control::StubInterface* stub_;
  Options options_;
  std::shared_ptr<spdlog::logger> logger_;

  std::atomic<bool> running_{false};
  std::size_t attempt_ = 0;

  std::mutex callback_mu_;
  ReadyCallback ready_callback_;
  MessageCallback message_callback_;
  StreamClosedCallback stream_closed_callback_;

  std::mutex write_mu_;
  std::deque<ops::v1::AgentToServer> pending_writes_;
  std::optional<ops::v1::AgentToServer> current_write_;
  bool write_in_flight_ = false;

  std::mutex reactor_mu_;
  StreamReactor* reactor_ = nullptr;
};

}  // namespace zurg::agent

#endif  // ZURG_AGENT_CONTROL_CONTROL_STREAM_CLIENT_H_
