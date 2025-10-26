#include "zurg/auth/auth_manager.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

#include <spdlog/sinks/basic_file_sink.h>

#include "zurg/agent/events.h"

namespace zurg::auth {

namespace {

namespace proto = ::auth::v1;

std::string ModeToString(AuthMode mode) {
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

std::string StateToString(AuthState state) {
  switch (state) {
    case AuthState::kOnline:
      return "ONLINE";
    case AuthState::kOffline:
      return "OFFLINE";
    case AuthState::kUnknown:
    default:
      return "UNKNOWN";
  }
}

}  // namespace

AuthManager::AuthManager(const std::string& server_address, const std::string& client_id)
    : client_(std::make_unique<AuthClient>(server_address, client_id)),
      mode_(AuthMode::kUnknown),
      auth_state_(AuthState::kUnknown) {
  client_->SetMessageCallback(
      [this](const proto::LicenseResp& message) { HandleMessage(message); });
  client_->SetConnectionCallback([this](bool connected) { HandleConnection(connected); });

  try {
    auto logger = spdlog::get("auth_manager");
    if (!logger) {
      logger = spdlog::basic_logger_mt("auth_manager", "auth_manager.log");
      logger->set_level(spdlog::level::debug);
      logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v");
      spdlog::flush_every(std::chrono::seconds(3));
    }
    logger->info("AuthManager created with server_address: {}, client_id: {}", server_address,
                 client_id);
  } catch (const std::exception& e) {
    std::cerr << "Failed to initialize logger: " << e.what() << std::endl;
  }

  UpdateAuthState(AuthState::kUnknown, true);
}

AuthManager::~AuthManager() {
  spdlog::info("AuthManager destroyed");
  Stop();
}

void AuthManager::Start() {
  auto mode = mode_.load();
  spdlog::info("Starting AuthManager in {} mode", ModeToString(mode));

  if (mode == AuthMode::kUnknown) {
    mode = AuthMode::kOnline;
    mode_.store(mode);
    spdlog::info("Mode was UNKNOWN, setting to ONLINE for initial connection attempt");
  }

  if (mode == AuthMode::kOnline) {
    client_->Start();
  } else if (mode == AuthMode::kOffline) {
    CheckOfflineAuthorization();
  }
}

void AuthManager::Stop() {
  spdlog::info("Stopping AuthManager");
  if (mode_.load() == AuthMode::kOnline && client_) {
    client_->Stop();
  }
}

void AuthManager::SetPersistenceFile(const std::string& file_path) {
  persistence_file_ = file_path;
  spdlog::info("Persistence file set to: {}", file_path);
}

void AuthManager::SetMode(AuthMode mode) {
  mode_.store(mode);
  spdlog::info("Mode changed to: {}", ModeToString(mode));
}

AuthMode AuthManager::GetMode() const { return mode_.load(); }

AuthState AuthManager::GetAuthState() const { return auth_state_.load(); }

bool AuthManager::CheckAuthorization() {
  auto mode = mode_.load();
  spdlog::info("Checking authorization in {} mode", ModeToString(mode));

  if (mode == AuthMode::kOnline) {
    return CheckOnlineAuthorization();
  }
  if (mode == AuthMode::kOffline) {
    return CheckOfflineAuthorization();
  }

  spdlog::warn("Authorization check in UNKNOWN mode, returning false");
  return false;
}

bool AuthManager::CheckOfflineAuthorization() {
  if (persistence_file_.empty()) {
    spdlog::error("Persistence file not set, cannot check offline authorization");
    UpdateAuthState(AuthState::kOffline);
    return false;
  }

  try {
    if (!std::filesystem::exists(persistence_file_)) {
      spdlog::error("Persistence file does not exist: {}", persistence_file_);
      UpdateAuthState(AuthState::kOffline);
      return false;
    }

    std::ifstream file(persistence_file_, std::ios::binary);
    if (!file.is_open()) {
      spdlog::error("Failed to open persistence file: {}", persistence_file_);
      UpdateAuthState(AuthState::kOffline);
      return false;
    }

    spdlog::info("Checking offline authorization using file: {}", persistence_file_);

    std::string serialized_data((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
    file.close();

    if (serialized_data.empty()) {
      spdlog::error("No data found in persistence file");
      UpdateAuthState(AuthState::kOffline);
      return false;
    }

    proto::LicenseResp message;
    if (!message.ParseFromString(serialized_data)) {
      spdlog::error("Failed to parse LicenseResp from file data");
      UpdateAuthState(AuthState::kOffline);
      return false;
    }

    auto now = std::time(nullptr);
    bool has_valid_asset = false;
    for (const auto& asset : message.assets()) {
      if (IsAssetValid(asset, now)) {
        has_valid_asset = true;
        spdlog::info("Asset {} is authorized, expires at: {}", asset.sn(), asset.expire_at());
      } else {
        spdlog::warn("Asset {} authorization expired or invalid", asset.sn());
      }
    }

    if (has_valid_asset) {
      spdlog::info("Offline authorization check: AUTHORIZED");
      UpdateAuthState(AuthState::kOnline);
      return true;
    }

    spdlog::warn("Offline authorization check: NOT AUTHORIZED (no valid assets)");
    UpdateAuthState(AuthState::kOffline);
    return false;
  } catch (const std::exception& e) {
    spdlog::error("Failed to check offline authorization: {}", e.what());
    UpdateAuthState(AuthState::kOffline);
    return false;
  }
}

bool AuthManager::CheckOnlineAuthorization() {
  auto state = auth_state_.load();
  spdlog::info("Online authorization check: {}", StateToString(state));
  return state == AuthState::kOnline;
}

void AuthManager::HandleMessage(const proto::LicenseResp& message) {
  spdlog::info("Handling message in AuthManager, request_id: {}", message.request_id());

  PersistMessage(message);

  if (mode_.load() == AuthMode::kOnline) {
    auto now = std::time(nullptr);
    bool authorized = false;

    for (const auto& asset : message.assets()) {
      if (IsAssetValid(asset, now)) {
        authorized = true;
        break;
      }
    }

    if (authorized) {
      UpdateAuthState(AuthState::kOnline);
      spdlog::info("Updated auth state to ONLINE based on received message");
    } else {
      UpdateAuthState(AuthState::kOffline);
      spdlog::warn("Updated auth state to OFFLINE based on received message");
    }
  }
}

void AuthManager::HandleConnection(bool connected) {
  spdlog::info("Handling connection status change: {}", connected ? "CONNECTED" : "DISCONNECTED");

  if (connected) {
    UpdateAuthState(AuthState::kOnline);
    mode_.store(AuthMode::kOnline);
    spdlog::info("Client connected, auth state set to ONLINE, mode set to ONLINE");
  } else {
    UpdateAuthState(AuthState::kOffline);
    if (mode_.load() == AuthMode::kOnline) {
      mode_.store(AuthMode::kOffline);
      spdlog::info("Client disconnected, mode set to OFFLINE");
    } else {
      spdlog::info("Client disconnected, mode remains {}", ModeToString(mode_.load()));
    }
  }
}

void AuthManager::UpdateAuthState(AuthState new_state, bool force) {
  auto previous = auth_state_.exchange(new_state);
  if (force || previous != new_state) {
    zurg::agent::events::AuthStateChangedEvent event{new_state};
    zurg::agent::events::GlobalEventBus().Publish(event);
  }
}

void AuthManager::PersistMessage(const proto::LicenseResp& message) {
  if (persistence_file_.empty()) {
    return;
  }

  try {
    std::string serialized_data;
    if (!message.SerializeToString(&serialized_data)) {
      spdlog::error("Failed to serialize LicenseResp to string");
      return;
    }

    std::lock_guard<std::mutex> lock(persistence_mutex_);
    std::ofstream file(persistence_file_, std::ios::binary);
    if (file.is_open()) {
      file << serialized_data;
      file.close();
      spdlog::debug("Message persisted to file: {}", persistence_file_);
    } else {
      spdlog::error("Failed to open persistence file: {}", persistence_file_);
    }
  } catch (const std::exception& e) {
    spdlog::error("Failed to persist message to file: {}", e.what());
  }
}

bool AuthManager::IsAssetValid(const proto::Asset& asset, std::time_t now) {
  if (asset.is_expired()) {
    return false;
  }

  if (asset.expire_at() > 0 && now >= asset.expire_at()) {
    return false;
  }

  if (asset.remaining_valid_timestamp() <= 0 && asset.expire_at() == 0) {
    return false;
  }

  return true;
}

}  // namespace zurg::auth
