#pragma once

#include <vector>

#include "events/event_types.h"

class EventQueue {
  public:
    void Push(const Event& event);
    const std::vector<Event>& GetEvents() const;
    void Clear();

  private:
    std::vector<Event> events_;
};
