#pragma once

#include <string>

#include <raylib.h>

#include "events/event_queue.h"
#include "game/game_state.h"
#include "game/game_types.h"

class BaseSpell {
  public:
    BaseSpell(Vector2 cast_position, int caster_player_id, SpellDirection direction)
        : cast_position_(cast_position), caster_player_id_(caster_player_id), direction_(direction) {}

    virtual ~BaseSpell() = default;

    virtual std::string GetName() const = 0;
    virtual void Cast(GameState& state, EventQueue& event_queue) = 0;

  protected:
    Vector2 cast_position_ = {0.0f, 0.0f};
    int caster_player_id_ = -1;
    SpellDirection direction_ = SpellDirection::Right;
};
