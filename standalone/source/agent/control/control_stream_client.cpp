#include "control/control_stream_client.h"

#include <utility>

namespace zurg::agent {

class ControlStreamClient::StreamReactor
    : public grpc::ClientBidiReactor<ops::v1::AgentToServer, ops::v1::ServerToAgent> {
 public:
  StreamReactor(ControlStreamClient* parent, ops::v1::Control::StubInterface* stub,
                grpc::ClientContext* ctx)
      : parent_(parent), context_(ctx) {
    stub->experimental_async()->Connect(context_, this);
  }

  void Start() {
    StartCall();
    parent_->OnStreamReady(this);
    StartRead(&incoming_);
  }

  grpc::Status Wait() {
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait(lock, [&] { return done_; });
    return status_;
  }

  void Stop() { context_->TryCancel(); }

  void OnReadDone(bool ok) override {
    parent_->OnMessage(incoming_, ok);
    if (ok) {
      StartRead(&incoming_);
    }
  }

  void OnWriteDone(bool ok) override { parent_->OnWriteFinished(ok); }

  void OnDone(const ::grpc::Status& status) override {
    parent_->OnStreamClosed(status);
    {
      std::lock_guard<std::mutex> lock(mu_);
      done_ = true;
      status_ = status;
    }
    cv_.notify_all();
  }

 private:
  ControlStreamClient* parent_;
  grpc::ClientContext* context_;
  ops::v1::ServerToAgent incoming_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool done_ = false;
  grpc::Status status_;
};

ControlStreamClient::ControlStreamClient(ops::v1::Control::StubInterface* stub, Options options,
                                         std::shared_ptr<spdlog::logger> logger)
    : stub_(stub), options_(std::move(options)), logger_(std::move(logger)) {
  if (!options_.should_run) {
    options_.should_run = [] { return true; };
  }
  if (!options_.backoff_fn) {
    options_.backoff_fn = [](std::size_t) { return std::chrono::milliseconds{0}; };
  }
  if (!options_.sleep_fn) {
    options_.sleep_fn = [](std::chrono::milliseconds delay) { std::this_thread::sleep_for(delay); };
  }
}

ControlStreamClient::~ControlStreamClient() { Stop(); }

void ControlStreamClient::SetMessageCallback(MessageCallback cb) {
  std::lock_guard<std::mutex> lock(callback_mu_);
  message_callback_ = std::move(cb);
}

void ControlStreamClient::SetReadyCallback(ReadyCallback cb) {
  std::lock_guard<std::mutex> lock(callback_mu_);
  ready_callback_ = std::move(cb);
}

void ControlStreamClient::SetStreamClosedCallback(StreamClosedCallback cb) {
  std::lock_guard<std::mutex> lock(callback_mu_);
  stream_closed_callback_ = std::move(cb);
}

void ControlStreamClient::Run() {
  running_.store(true);
  attempt_ = 0;
  while (ShouldContinue()) {
    grpc::ClientContext ctx;
    if (logger_) {
      logger_->info("connecting to control stream (attempt={})", attempt_ + 1);
    }
    StreamReactor reactor(this, stub_, &ctx);
    {
      std::lock_guard<std::mutex> lock(reactor_mu_);
      reactor_ = &reactor;
    }
    reactor.Start();
    grpc::Status status = reactor.Wait();
    {
      std::lock_guard<std::mutex> lock(reactor_mu_);
      if (reactor_ == &reactor) {
        reactor_ = nullptr;
      }
    }

    if (!ShouldContinue()) {
      break;
    }

    ++attempt_;
    auto delay =
        options_.backoff_fn ? options_.backoff_fn(attempt_) : std::chrono::milliseconds{0};
    if (logger_) {
      logger_->warn("stream closed (code={}, message='{}'), reconnecting in {} ms",
                    static_cast<int>(status.error_code()), status.error_message(), delay.count());
    }
    if (delay.count() > 0) {
      if (options_.sleep_fn) {
        options_.sleep_fn(delay);
      } else {
        std::this_thread::sleep_for(delay);
      }
    }
  }
  running_.store(false);
  if (logger_) {
    logger_->info("control stream loop stopped");
  }
}

void ControlStreamClient::Stop() {
  running_.store(false);
  StreamReactor* reactor = nullptr;
  {
    std::lock_guard<std::mutex> lock(reactor_mu_);
    reactor = reactor_;
  }
  if (reactor) {
    reactor->Stop();
  }
}

void ControlStreamClient::CancelStream() {
  StreamReactor* reactor = nullptr;
  {
    std::lock_guard<std::mutex> lock(reactor_mu_);
    reactor = reactor_;
  }
  if (reactor) {
    reactor->Stop();
  }
}

void ControlStreamClient::EnqueueWrite(ops::v1::AgentToServer msg) {
  if (options_.on_send) {
    options_.on_send(msg);
  }
  std::lock_guard<std::mutex> lock(write_mu_);
  pending_writes_.push_back(std::move(msg));
  MaybeStartWriteLocked();
}

bool ControlStreamClient::ShouldContinue() const {
  if (!running_.load()) {
    return false;
  }
  if (options_.should_run && !options_.should_run()) {
    return false;
  }
  return true;
}

void ControlStreamClient::OnStreamReady(StreamReactor* reactor) {
  {
    std::lock_guard<std::mutex> lock(reactor_mu_);
    reactor_ = reactor;
  }
  {
    std::lock_guard<std::mutex> lock(callback_mu_);
    if (ready_callback_) {
      ready_callback_();
    }
  }
  MaybeStartWriteLocked();
}

void ControlStreamClient::OnWriteFinished(bool ok) {
  std::lock_guard<std::mutex> lock(write_mu_);
  if (logger_) {
    logger_->debug("write finished ok={} pending={} in_flight_before={}", ok,
                   pending_writes_.size(), write_in_flight_);
  }
  write_in_flight_ = false;
  current_write_.reset();
  if (!ok) {
    pending_writes_.clear();
  }
  MaybeStartWriteLocked();
}

void ControlStreamClient::OnMessage(const ops::v1::ServerToAgent& msg, bool ok) {
  std::lock_guard<std::mutex> lock(callback_mu_);
  if (message_callback_) {
    message_callback_(msg, ok);
  }
}

void ControlStreamClient::OnStreamClosed(const grpc::Status& status) {
  {
    std::lock_guard<std::mutex> lock(write_mu_);
    write_in_flight_ = false;
    current_write_.reset();
    pending_writes_.clear();
  }
  {
    std::lock_guard<std::mutex> lock(reactor_mu_);
    reactor_ = nullptr;
  }
  std::lock_guard<std::mutex> lock(callback_mu_);
  if (stream_closed_callback_) {
    stream_closed_callback_(status);
  }
}

void ControlStreamClient::MaybeStartWriteLocked() {
  if (write_in_flight_ || pending_writes_.empty()) {
    return;
  }
  StreamReactor* reactor = nullptr;
  {
    std::lock_guard<std::mutex> lock(reactor_mu_);
    reactor = reactor_;
  }
  if (!reactor) {
    return;
  }
  current_write_.emplace(std::move(pending_writes_.front()));
  pending_writes_.pop_front();
  write_in_flight_ = true;
  reactor->StartWrite(&*current_write_);
}

}  // namespace zurg::agent
