#include "zurg/auth/auth_client.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

using zurg::auth::AuthClient;
using zurg::auth::AuthMode;

TEST(ConnectionTest, ClientStartsAndStops) {
  AuthClient client("localhost:50051", "test-client");
  EXPECT_NO_THROW(client.Start());
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_NO_THROW(client.Stop());
}

TEST(ConnectionTest, ConnectionCallbackCanBeRegistered) {
  AuthClient client("localhost:50051", "test-client");

  bool callback_called = false;
  bool connection_status = false;
  client.SetConnectionCallback([&](bool connected) {
    callback_called = true;
    connection_status = connected;
  });

  EXPECT_FALSE(callback_called);
  EXPECT_FALSE(connection_status);
}

TEST(ConnectionTest, ModeTransitions) {
  AuthClient client("localhost:50051", "test-client");
  EXPECT_EQ(client.GetMode(), AuthMode::kOnline);

  client.SetMode(AuthMode::kOffline);
  EXPECT_EQ(client.GetMode(), AuthMode::kOffline);

  client.SetMode(AuthMode::kOnline);
  EXPECT_EQ(client.GetMode(), AuthMode::kOnline);
}
