#pragma once

#include <string>
#include <spdlog/spdlog.h>
#include <eventpp/eventqueue.h>

namespace greeter {

  /**  Language codes to be used with the Greeter class */
  enum class LanguageCode { EN, DE, ES, FR };

  /**
   * @brief A class for saying hello in multiple languages
   */
  class Greeter {
    std::string name;

  public:
    /**
     * @brief Creates a new greeter
     * @param name the name to greet
     */
    Greeter(std::string name);

    /**
     * @brief Creates a localized string containing the greeting
     * @param lang the language to greet in
     * @return a string containing the greeting
     */
    std::string greet(LanguageCode lang = LanguageCode::EN) const;
    
    // Example usage of spdlog
    void log_greeting(LanguageCode lang = LanguageCode::EN) const;
    
    // Example usage of eventpp
    using EventQueue = eventpp::EventQueue<int, void(const std::string&)>;
    EventQueue& get_event_queue() const;

  private:
    mutable EventQueue event_queue_;
  };

}  // namespace greeter