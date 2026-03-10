#pragma once

#include "modes/base_mode.h"

class MostKillsMode : public BaseMode {
  public:
    std::string GetUiName() const override;
    void Update(GameState& state, EventQueue& event_queue, float dt) override;
    bool IsFinished(const GameState& state) const override;
    int GetWinningTeam(const GameState& state) const override;
};
