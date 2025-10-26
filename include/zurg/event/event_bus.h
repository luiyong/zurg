#pragma once

#include <any>
#include <functional>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

#include <eventpp/eventdispatcher.h>

#include "zurg/event/agent_events.h"

namespace zurg::agent::events {

class EventBus {
 public:
  using Dispatcher = eventpp::EventDispatcher<EventType, void(const std::any&)>;

  class SubscriptionToken {
   public:
    SubscriptionToken() = default;
    SubscriptionToken(EventType type, EventBus* bus, Dispatcher::Handle handle)
        : type_(type), bus_(bus), handle_(handle), active_(true) {}
    SubscriptionToken(const SubscriptionToken&) = delete;
    SubscriptionToken& operator=(const SubscriptionToken&) = delete;
    SubscriptionToken(SubscriptionToken&& other) noexcept { MoveFrom(std::move(other)); }
    SubscriptionToken& operator=(SubscriptionToken&& other) noexcept {
      if (this != &other) {
        Unsubscribe();
        MoveFrom(std::move(other));
      }
      return *this;
    }
    ~SubscriptionToken() { Unsubscribe(); }

    void Unsubscribe() {
      if (active_ && bus_) {
        bus_->Unsubscribe(type_, handle_);
        active_ = false;
        bus_ = nullptr;
      }
    }

    explicit operator bool() const { return active_; }

   private:
    void MoveFrom(SubscriptionToken&& other) {
      type_ = other.type_;
      bus_ = other.bus_;
      handle_ = other.handle_;
      active_ = other.active_;
      other.active_ = false;
      other.bus_ = nullptr;
    }

    EventType type_{EventType::kAuthStateChanged};
    EventBus* bus_ = nullptr;
    Dispatcher::Handle handle_{};
    bool active_ = false;
  };

  template <typename Event, typename Callback>
  SubscriptionToken Subscribe(Callback&& callback) {
    auto wrapper = [func = std::forward<Callback>(callback)](const std::any& payload) {
      const auto& event = std::any_cast<const Event&>(payload);
      func(event);
    };
    std::lock_guard<std::mutex> lock(mu_);
    auto handle = dispatcher_.appendListener(Event::kType, std::move(wrapper));
    return SubscriptionToken{Event::kType, this, handle};
  }

  template <typename Event>
  void Publish(const Event& event) {
    std::lock_guard<std::mutex> lock(mu_);
    dispatcher_.dispatch(Event::kType, std::any(event));
    last_events_[Event::kType] = event;
  }

  template <typename Event>
  std::optional<Event> LastEvent() const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = last_events_.find(Event::kType);
    if (it == last_events_.end()) return std::nullopt;
    return std::any_cast<Event>(it->second);
  }

  void Unsubscribe(EventType type, Dispatcher::Handle handle) {
    std::lock_guard<std::mutex> lock(mu_);
    dispatcher_.removeListener(type, handle);
  }

 private:
    mutable std::mutex mu_;
    Dispatcher dispatcher_;
    std::unordered_map<EventType, std::any> last_events_;
};

}  // namespace zurg::agent::events

