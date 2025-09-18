#include <gtest/gtest.h>
#include <greeter/greeter.h>

using namespace greeter;

TEST(GreeterTest, Construct) {
  Greeter greeter("World");
  EXPECT_EQ(greeter.greet(), "Hello, World!");
}

TEST(GreeterTest, Languages) {
  Greeter greeter("World");
  EXPECT_EQ(greeter.greet(LanguageCode::EN), "Hello, World!");
  EXPECT_EQ(greeter.greet(LanguageCode::DE), "Hallo World!");
  EXPECT_EQ(greeter.greet(LanguageCode::ES), "¡Hola World!");
  EXPECT_EQ(greeter.greet(LanguageCode::FR), "Bonjour World!");
}

TEST(GreeterTest, LogGreetingNoCrash) {
  Greeter greeter("Test");
  greeter.log_greeting();
  SUCCEED();
}

TEST(GreeterTest, EventQueue) {
  Greeter greeter("EventTest");
  auto& queue = greeter.get_event_queue();

  bool event_received = false;
  queue.appendListener(1, [&event_received](const std::string& message) {
    event_received = true;
    EXPECT_EQ(message, "Hello, EventTest!");
  });

  queue.enqueue(1, "Hello, EventTest!");
  queue.process();

  EXPECT_TRUE(event_received);
}
