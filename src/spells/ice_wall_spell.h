#pragma once

#include "game/game_types.h"
#include "spells/base_spell.h"

class IceWallSpell : public BaseSpell {
  public:
    IceWallSpell(const GridCoord& cast_cell, int caster_player_id, SpellDirection direction)
        : BaseSpell({0.0f, 0.0f}, caster_player_id, direction), cast_cell_(cast_cell) {}

    std::string GetName() const override;
    void Cast(GameState& state, EventQueue& event_queue) override;

  private:
    GridCoord cast_cell_;
};
