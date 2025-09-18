#include <fmt/format.h>
#include <greeter/greeter.h>

using namespace greeter;

Greeter::Greeter(std::string _name) : name(std::move(_name)) {
    // Initialize spdlog
    spdlog::info("Greeter created for name: {}", name);
    
    // Example usage of eventpp
    event_queue_.appendListener(1, [](const std::string& message) {
        spdlog::info("Event received: {}", message);
    });
}

std::string Greeter::greet(LanguageCode lang) const {
  switch (lang) {
    default:
    case LanguageCode::EN:
      return fmt::format("Hello, {}!", name);
    case LanguageCode::DE:
      return fmt::format("Hallo {}!", name);
    case LanguageCode::ES:
      return fmt::format("¡Hola {}!", name);
    case LanguageCode::FR:
      return fmt::format("Bonjour {}!", name);
  }
}

void Greeter::log_greeting(LanguageCode lang) const {
    std::string greeting = greet(lang);
    spdlog::info("Greeting: {}", greeting);
    
    // Emit an event
    event_queue_.enqueue(1, greeting);
    event_queue_.process();
}

Greeter::EventQueue& Greeter::get_event_queue() const {
    return event_queue_;
}