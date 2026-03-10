#include "events/event_queue.h"

void EventQueue::Push(const Event& event) { events_.push_back(event); }

const std::vector<Event>& EventQueue::GetEvents() const { return events_; }

void EventQueue::Clear() { events_.clear(); }
