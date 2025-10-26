#include "zurg/agent/events.h"

namespace zurg::agent::events {

EventBus& GlobalEventBus() {
  static EventBus bus;
  return bus;
}

}  // namespace zurg::agent::events

