#ifndef ZURG_AUTH_AUTH_MANAGER_H_
#define ZURG_AUTH_AUTH_MANAGER_H_

#include <atomic>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>

#include "auth/auth.grpc.pb.h"
#include "zurg/auth/auth_client.h"

namespace zurg::auth {

namespace proto = ::auth::v1;

enum class AuthState {
  kUnknown,
  kOnline,
  kOffline,
};

class AuthManager {
 public:
  AuthManager(const std::string& server_address, const std::string& client_id);
  ~AuthManager();

  void Start();
  void Stop();

  void SetPersistenceFile(const std::string& file_path);
  void SetMode(AuthMode mode);

  AuthMode GetMode() const;
  AuthState GetAuthState() const;

  bool CheckAuthorization();

 private:
  bool CheckOfflineAuthorization();
  bool CheckOnlineAuthorization();

  void HandleMessage(const proto::LicenseResp& message);
  void HandleConnection(bool connected);
  void PersistMessage(const proto::LicenseResp& message);
  void UpdateAuthState(AuthState new_state, bool force = false);

  static bool IsAssetValid(const proto::Asset& asset, std::time_t now);

  std::unique_ptr<AuthClient> client_;
  std::string persistence_file_;

  std::atomic<AuthMode> mode_;
  std::atomic<AuthState> auth_state_;

  std::mutex persistence_mutex_;
};

}  // namespace zurg::auth

#endif  // ZURG_AUTH_AUTH_MANAGER_H_
