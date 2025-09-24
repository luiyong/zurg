#include "zurg/logger_manager.h"

#include <gtest/gtest.h>
#include <spdlog/sinks/ringbuffer_sink.h>

#include <algorithm>

namespace zurg::logging {

namespace {

class LoggerManagerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    LoggerManager::shutdown();
    LoggerManager::init(LoggerConfig{
        .default_level = spdlog::level::info,
        .pattern = std::string{"[%l] %v"},
    });
    sink_ = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(128);
    LoggerManager::add_sink(sink_);
  }

  void TearDown() override { LoggerManager::shutdown(); }

  LoggerManager::sink_ptr sink_;
};

}  // namespace

TEST_F(LoggerManagerTest, CreatesAndCachesLogger) {
  auto first = LoggerManager::logger("core");
  ASSERT_TRUE(first);
  EXPECT_EQ(first->level(), spdlog::level::info);
  ASSERT_FALSE(first->sinks().empty());
  EXPECT_EQ(first->sinks().front(), sink_);

  auto second = LoggerManager::logger("core");
  EXPECT_EQ(first.get(), second.get());

  std::weak_ptr<spdlog::logger> weak = first;
  first.reset();
  second.reset();

  EXPECT_TRUE(weak.expired());
  auto infos = LoggerManager::list_loggers();
  EXPECT_TRUE(std::none_of(infos.begin(), infos.end(), [](const LoggerInfo& info) {
    return info.name == "core" && info.active;
  }));
}

TEST_F(LoggerManagerTest, SetLevelUpdatesExistingLogger) {
  auto log = LoggerManager::logger("net.http");
  ASSERT_TRUE(log);
  EXPECT_EQ(log->level(), spdlog::level::info);

  LoggerManager::set_level("net.http", spdlog::level::debug);
  EXPECT_EQ(log->level(), spdlog::level::debug);
}

TEST_F(LoggerManagerTest, SetLevelBeforeCreationIsApplied) {
  LoggerManager::set_level("pending", spdlog::level::warn);
  auto log = LoggerManager::logger("pending");
  ASSERT_TRUE(log);
  EXPECT_EQ(log->level(), spdlog::level::warn);
}

TEST_F(LoggerManagerTest, ListLoggersReturnsActiveEntries) {
  auto a = LoggerManager::logger("alpha");
  auto b = LoggerManager::logger("beta");
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);

  auto infos = LoggerManager::list_loggers();
  EXPECT_EQ(infos.size(), 2);
  EXPECT_TRUE(std::all_of(infos.begin(), infos.end(), [](const LoggerInfo& info) {
    return info.active;
  }));
}

TEST_F(LoggerManagerTest, ShutdownClearsState) {
  ASSERT_TRUE(LoggerManager::logger("gamma"));
  LoggerManager::shutdown();
  auto infos = LoggerManager::list_loggers();
  EXPECT_TRUE(infos.empty());
  auto after = LoggerManager::logger("gamma");
  EXPECT_FALSE(after);
}

}  // namespace zurg::logging

