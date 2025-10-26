#ifndef ZURG_AUTH_AUTH_CLIENT_H_
#define ZURG_AUTH_AUTH_CLIENT_H_

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <spdlog/logger.h>
#include <spdlog/spdlog.h>

#include "auth/auth.grpc.pb.h"

namespace zurg::auth {

namespace proto = ::auth::v1;

enum class AuthMode {
  kUnknown,
  kOnline,
  kOffline,
};

class AuthClient {
 public:
  AuthClient(const std::string& server_address, const std::string& client_id);
  ~AuthClient();

  void Start();
  void Stop();

  void SetMessageCallback(std::function<void(const proto::LicenseResp&)> callback);
  void SetConnectionCallback(std::function<void(bool)> callback);

  bool Reconnect();

  void SetMode(AuthMode mode);
  AuthMode GetMode() const;

 private:
  std::shared_ptr<grpc::Channel> CreateSecureChannel();
  void StreamLoop();
  void InitializeLogger();
  void NotifyReconnect();

  std::string server_address_;
  std::string client_id_;
  std::unique_ptr<proto::Authorization::Stub> stub_;
  std::atomic<bool> running_;
  std::thread stream_thread_;
  std::function<void(const proto::LicenseResp&)> message_callback_;
  std::function<void(bool)> connection_callback_;

  std::atomic<int> reconnect_attempts_;
  static constexpr int kReconnectDelayMs = 5000;

  std::atomic<AuthMode> mode_;

  std::shared_ptr<spdlog::logger> logger_;

  std::mutex reconnect_mutex_;
  std::condition_variable reconnect_cv_;
  std::atomic<bool> reconnect_needed_;

  std::mutex reactor_mutex_;
  class AuthStreamReactor;
  AuthStreamReactor* active_reactor_ = nullptr;

  std::string root_cert_path_;
  std::string client_cert_path_;
  std::string client_key_path_;

  friend class AuthStreamReactor;
};

}  // namespace zurg::auth

#endif  // ZURG_AUTH_AUTH_CLIENT_H_
