#pragma once

#include "spells/base_spell.h"

class FireBoltSpell : public BaseSpell {
  public:
    FireBoltSpell(Vector2 cast_position, int caster_player_id, SpellDirection direction)
        : BaseSpell(cast_position, caster_player_id, direction) {}

    std::string GetName() const override;
    void Cast(GameState& state, EventQueue& event_queue) override;
};
