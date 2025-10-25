#include "zurg/auth/auth_manager.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <ctime>

using zurg::auth::AuthManager;
using zurg::auth::AuthMode;
using zurg::auth::AuthState;

namespace {

auth::v1::LicenseResp BuildLicenseResp(bool asset_expired) {
  auth::v1::LicenseResp message;
  message.set_request_id("test-request-123");

  auto* asset = message.add_assets();
  asset->set_sn("SN123456");
  asset->set_name("Test Asset");
  asset->set_created_at(std::time(nullptr) - 60);
  if (asset_expired) {
    asset->set_is_expired(true);
    asset->set_expire_at(std::time(nullptr) - 10);
    asset->set_remaining_valid_timestamp(0);
  } else {
    asset->set_is_expired(false);
    asset->set_expire_at(std::time(nullptr) + 3600);
    asset->set_remaining_valid_timestamp(3600);
  }

  return message;
}

void WriteLicenseToFile(const std::string& path, const auth::v1::LicenseResp& message) {
  std::string serialized;
  ASSERT_TRUE(message.SerializeToString(&serialized));
  std::ofstream file(path, std::ios::binary);
  ASSERT_TRUE(file.is_open());
  file << serialized;
  file.close();
}

}  // namespace

TEST(AuthManagerTest, ConstructorAndDestructor) {
  ASSERT_NO_THROW({
    AuthManager manager("localhost:50051", "test-client");
  });
}

TEST(AuthManagerTest, SetPersistenceFile) {
  AuthManager manager("localhost:50051", "test-client");
  manager.SetPersistenceFile("test_persistence.dat");
}

TEST(AuthManagerTest, SetAndGetMode) {
  AuthManager manager("localhost:50051", "test-client");

  EXPECT_EQ(manager.GetMode(), AuthMode::kUnknown);

  manager.SetMode(AuthMode::kOffline);
  EXPECT_EQ(manager.GetMode(), AuthMode::kOffline);

  manager.SetMode(AuthMode::kOnline);
  EXPECT_EQ(manager.GetMode(), AuthMode::kOnline);
}

TEST(AuthManagerTest, StartAndStopOnline) {
  AuthManager manager("localhost:50051", "test-client");
  manager.SetMode(AuthMode::kOnline);

  EXPECT_NO_THROW(manager.Start());
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_NO_THROW(manager.Stop());
}

TEST(AuthManagerTest, OfflineAuthorizationSuccess) {
  AuthManager manager("localhost:50051", "test-client");

  const std::string test_file = "test_auth_success.bin";
  manager.SetPersistenceFile(test_file);
  auto message = BuildLicenseResp(/*asset_expired=*/false);
  WriteLicenseToFile(test_file, message);

  manager.SetMode(AuthMode::kOffline);
  EXPECT_TRUE(manager.CheckAuthorization());
  EXPECT_EQ(manager.GetAuthState(), AuthState::kOnline);

  std::filesystem::remove(test_file);
}

TEST(AuthManagerTest, OfflineAuthorizationExpiredAsset) {
  AuthManager manager("localhost:50051", "test-client");

  const std::string test_file = "test_auth_expired.bin";
  manager.SetPersistenceFile(test_file);
  auto message = BuildLicenseResp(/*asset_expired=*/true);
  WriteLicenseToFile(test_file, message);

  manager.SetMode(AuthMode::kOffline);
  EXPECT_FALSE(manager.CheckAuthorization());
  EXPECT_EQ(manager.GetAuthState(), AuthState::kOffline);

  std::filesystem::remove(test_file);
}

TEST(AuthManagerTest, OfflineAuthorizationMissingFile) {
  AuthManager manager("localhost:50051", "test-client");
  manager.SetPersistenceFile("nonexistent_auth_file.bin");

  manager.SetMode(AuthMode::kOffline);
  EXPECT_FALSE(manager.CheckAuthorization());
}
