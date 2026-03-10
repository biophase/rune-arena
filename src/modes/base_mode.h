#pragma once

#include <string>

#include "events/event_queue.h"
#include "game/game_state.h"

class BaseMode {
  public:
    virtual ~BaseMode() = default;

    virtual std::string GetUiName() const = 0;
    virtual void Update(GameState& state, EventQueue& event_queue, float dt) = 0;
    virtual bool IsFinished(const GameState& state) const = 0;
    virtual int GetWinningTeam(const GameState& state) const = 0;
};
