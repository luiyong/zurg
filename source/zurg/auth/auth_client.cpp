#include "zurg/auth/auth_client.h"

#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

#include <chrono>
#include <iostream>
#include <thread>
#include <utility>

#include <spdlog/sinks/basic_file_sink.h>

namespace zurg::auth {

static std::string ModeToString(AuthMode mode) {
  switch (mode) {
    case AuthMode::kOnline:
      return "ONLINE";
    case AuthMode::kOffline:
      return "OFFLINE";
    case AuthMode::kUnknown:
    default:
      return "UNKNOWN";
  }
}

class AuthClient::AuthStreamReactor
    : public grpc::ClientReadReactor<proto::ServerMessage> {
 public:
  AuthStreamReactor(AuthClient* client, proto::Authorization::Stub* stub,
                    const proto::RegisterReq& request)
      : client_(client), stub_(stub), request_(request), finished_(false) {}

  void Start() {
    finished_.store(false);
    stub_->async()->EstablishAuthStream(&context_, &request_, this);
    StartCall();
    StartRead(&response_);

    if (client_->logger_) {
      client_->logger_->debug("AuthStreamReactor started");
    }
  }

  void Stop() {
    finished_.store(true);
    context_.TryCancel();
    if (client_->logger_) {
      client_->logger_->debug("AuthStreamReactor stopped");
    }
  }

  bool IsFinished() const { return finished_.load(); }

  void OnReadInitialMetadataDone(bool ok) override {
    if (client_->logger_) {
      client_->logger_->debug("Read initial metadata done, ok: {}", ok);
    }
  }

  void OnReadDone(bool ok) override {
    if (finished_) {
      return;
    }

    if (client_->logger_) {
      client_->logger_->debug("Read done, ok: {}", ok);
    }

    if (!ok) {
      return;
    }

    if (response_.has_license_resp() && client_->message_callback_) {
      if (client_->logger_) {
        client_->logger_->info("Received license response, request_id: {}",
                               response_.license_resp().request_id());
      }
      client_->message_callback_(response_.license_resp());
    }

    response_.Clear();
    if (!finished_) {
      StartRead(&response_);
    }
  }

  void OnDone(const grpc::Status& status) override {
    finished_.store(true);

    if (client_->logger_) {
      if (status.ok()) {
        client_->logger_->info("Connection closed normally by server");
      } else {
        client_->logger_->error("Connection closed with error: {}", status.error_message());
      }
    }

    if (client_->connection_callback_) {
      client_->connection_callback_(status.ok());
    }

    if (!status.ok() && client_->running_.load()) {
      client_->NotifyReconnect();
    }
  }

 private:
  AuthClient* client_;
  proto::Authorization::Stub* stub_;
  proto::RegisterReq request_;
  proto::ServerMessage response_;
  std::atomic<bool> finished_;
  grpc::ClientContext context_;
};

AuthClient::AuthClient(const std::string& server_address, const std::string& client_id)
    : server_address_(server_address),
      client_id_(client_id),
      running_(false),
      reconnect_attempts_(0),
      mode_(AuthMode::kOnline),
      reconnect_needed_(false) {
  InitializeLogger();

  auto channel = CreateSecureChannel();
  stub_ = proto::Authorization::NewStub(channel);

  if (logger_) {
    logger_->info("AuthClient created with server_address: {}, client_id: {}", server_address,
                  client_id);
  }
}

AuthClient::~AuthClient() {
  Stop();
  if (logger_) {
    logger_->info("AuthClient destroyed");
  }
}

void AuthClient::InitializeLogger() {
  try {
    auto existing_logger = spdlog::get("auth_client");
    if (existing_logger) {
      logger_ = existing_logger;
      logger_->info("Using existing logger");
    } else {
      logger_ = spdlog::basic_logger_mt("auth_client", "auth_client.log");
      logger_->set_level(spdlog::level::debug);
      logger_->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v");
      spdlog::flush_every(std::chrono::seconds(3));
      logger_->info("Logger initialized successfully");
    }
  } catch (const std::exception& e) {
    std::cerr << "Failed to initialize logger: " << e.what() << std::endl;
    logger_ = nullptr;
  }
}

std::shared_ptr<grpc::Channel> AuthClient::CreateSecureChannel() {
  auto creds = grpc::SslCredentials(grpc::SslCredentialsOptions());
  auto channel = grpc::CreateChannel(server_address_, creds);

  if (logger_) {
    if (channel) {
      logger_->info("Secure channel created successfully");
    } else {
      logger_->error("Failed to create secure channel");
    }
  }

  return channel;
}

void AuthClient::Start() {
  if (running_.exchange(true)) {
    if (logger_) {
      logger_->warn("Client is already running");
    }
    return;
  }

  if (logger_) {
    logger_->info("Starting client in {} mode", ModeToString(mode_));
  }

  if (mode_ == AuthMode::kOffline) {
    if (logger_) {
      logger_->info("Offline mode, not starting stream");
    }
    running_.store(false);
    return;
  }

  stream_thread_ = std::thread(&AuthClient::StreamLoop, this);
}

void AuthClient::Stop() {
  if (!running_.exchange(false)) {
    if (logger_) {
      logger_->debug("Client already stopped");
    }
    return;
  }

  {
    std::lock_guard<std::mutex> lock(reactor_mutex_);
    if (active_reactor_) {
      active_reactor_->Stop();
    }
  }

  {
    std::lock_guard<std::mutex> lock(reconnect_mutex_);
    reconnect_needed_.store(true);
  }
  reconnect_cv_.notify_all();

  if (stream_thread_.joinable()) {
    stream_thread_.join();
  }

  if (logger_) {
    logger_->info("Client stopped successfully");
  }
}

void AuthClient::SetMessageCallback(
    std::function<void(const proto::LicenseResp&)> callback) {
  message_callback_ = std::move(callback);
  if (logger_) {
    logger_->debug("Message callback set");
  }
}

void AuthClient::SetConnectionCallback(std::function<void(bool)> callback) {
  connection_callback_ = std::move(callback);
  if (logger_) {
    logger_->debug("Connection callback set");
  }
}

void AuthClient::SetMode(AuthMode mode) {
  mode_ = mode;
  if (logger_) {
    logger_->info("Mode changed to: {}", ModeToString(mode));
  }
}

AuthMode AuthClient::GetMode() const { return mode_; }

bool AuthClient::Reconnect() {
  try {
    if (logger_) {
      logger_->info("Attempting to reconnect...");
    }

    auto channel = CreateSecureChannel();
    if (!channel) {
      if (logger_) {
        logger_->error("Failed to create channel");
      }
      return false;
    }

    stub_ = proto::Authorization::NewStub(channel);
    if (!stub_) {
      if (logger_) {
        logger_->error("Failed to create stub");
      }
      return false;
    }

    reconnect_attempts_ = 0;
    if (logger_) {
      logger_->info("Reconnect successful");
    }
    return true;
  } catch (const std::exception& e) {
    if (logger_) {
      logger_->error("Reconnect failed: {}", e.what());
    }
    return false;
  }
}

void AuthClient::StreamLoop() {
  reconnect_attempts_ = 0;

  if (logger_) {
    logger_->info("Entering stream loop");
  }

  while (running_) {
    if (!stub_) {
      if (!Reconnect()) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        continue;
      }
    }

    proto::RegisterReq request;
    request.set_client_id(client_id_);

    if (logger_) {
      logger_->debug("Creating AuthStreamReactor for client: {}", client_id_);
    }

    auto reactor =
        std::make_unique<AuthClient::AuthStreamReactor>(this, stub_.get(), request);

    {
      std::lock_guard<std::mutex> lock(reactor_mutex_);
      active_reactor_ = reactor.get();
    }

    reconnect_needed_.store(false);
    reactor->Start();

    while (running_ && !reactor->IsFinished()) {
      std::unique_lock<std::mutex> lock(reconnect_mutex_);
      if (reconnect_cv_.wait_for(
              lock, std::chrono::milliseconds(100),
              [this] { return !running_.load() || reconnect_needed_.load(); })) {
        if (!running_) {
          break;
        }
        if (reconnect_needed_.exchange(false)) {
          reactor->Stop();
          break;
        }
      }
    }

    while (running_ && !reactor->IsFinished()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    {
      std::lock_guard<std::mutex> lock(reactor_mutex_);
      if (active_reactor_ == reactor.get()) {
        active_reactor_ = nullptr;
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    if (!running_) {
      break;
    }

    reconnect_attempts_++;
    if (logger_) {
      logger_->warn("Connection lost, attempting to reconnect (attempt {})",
                    reconnect_attempts_.load());
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(kReconnectDelayMs));

    if (!Reconnect() && logger_) {
      logger_->error("Reconnect failed, will retry");
    }
  }

  {
    std::lock_guard<std::mutex> lock(reactor_mutex_);
    active_reactor_ = nullptr;
  }

  if (logger_) {
    logger_->info("Exiting stream loop");
  }
}

void AuthClient::NotifyReconnect() {
  {
    std::lock_guard<std::mutex> lock(reconnect_mutex_);
    reconnect_needed_.store(true);
  }
  reconnect_cv_.notify_one();

  if (logger_) {
    logger_->debug("Reconnect notification sent");
  }
}

}  // namespace zurg::auth
