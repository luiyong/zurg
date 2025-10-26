#include "zurg/auth/auth_client.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

using zurg::auth::AuthClient;
using zurg::auth::AuthMode;

TEST(AuthClientTest, ConstructorAndDestructor) {
  ASSERT_NO_THROW({
    AuthClient client("localhost:50051", "test-client");
  });
}

TEST(AuthClientTest, SetMessageCallback) {
  AuthClient client("localhost:50051", "test-client");

  bool callback_called = false;
  auth::v1::LicenseResp received_resp;

  client.SetMessageCallback([&](const auth::v1::LicenseResp& resp) {
    callback_called = true;
    received_resp = resp;
  });

  EXPECT_FALSE(callback_called);

}

TEST(AuthClientTest, SetConnectionCallback) {
  AuthClient client("localhost:50051", "test-client");

  bool connection_callback_called = false;
  bool connection_status = false;

  client.SetConnectionCallback([&](bool connected) {
    connection_callback_called = true;
    connection_status = connected;
  });

  EXPECT_FALSE(connection_callback_called);
  EXPECT_FALSE(connection_status);
}

TEST(AuthClientTest, StartAndStop) {
  AuthClient client("localhost:50051", "test-client");

  EXPECT_NO_THROW(client.Start());
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_NO_THROW(client.Stop());
}

TEST(AuthClientTest, StartAndStopMultipleTimes) {
  AuthClient client("localhost:50051", "test-client");

  client.Start();
  client.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  client.Stop();
  client.Stop();
}

TEST(AuthClientTest, ReconnectDoesNotThrow) {
  AuthClient client("localhost:50051", "test-client");
  (void)client.Reconnect();
}

TEST(AuthClientTest, SetAndGetMode) {
  AuthClient client("localhost:50051", "test-client");

  EXPECT_EQ(client.GetMode(), AuthMode::kOnline);

  client.SetMode(AuthMode::kOffline);
  EXPECT_EQ(client.GetMode(), AuthMode::kOffline);

  client.SetMode(AuthMode::kOnline);
  EXPECT_EQ(client.GetMode(), AuthMode::kOnline);
}
