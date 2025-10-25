#include "zurg/auth/auth_client.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

using zurg::auth::AuthClient;

TEST(ReconnectTest, ReconnectCanBeInvoked) {
  AuthClient client("localhost:50051", "test-client");
  (void)client.Reconnect();
  client.Stop();
}

TEST(ReconnectTest, ReconnectAfterStart) {
  AuthClient client("localhost:50051", "test-client");
  client.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  (void)client.Reconnect();
  client.Stop();
}

TEST(ReconnectTest, MultipleStartStop) {
  AuthClient client("localhost:50051", "test-client");
  client.Start();
  client.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  client.Stop();
  client.Stop();
}
