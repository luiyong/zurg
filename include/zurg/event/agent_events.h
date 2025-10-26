#pragma once

#include <cstdint>

#include "zurg/auth/auth_manager.h"

namespace zurg::agent::events {

enum class EventType : std::uint32_t {
  kAuthStateChanged = 1,
};

struct AuthStateChangedEvent {
  static constexpr EventType kType = EventType::kAuthStateChanged;
  zurg::auth::AuthState state;
};

}  // namespace zurg::agent::events

