#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "events/event_queue.h"
#include "game/game_state.h"
#include "game/game_types.h"

struct SpellRuntimeMatch {
    std::string spell_name;
    int caster_player_id = -1;
    int caster_team = 0;
    SpellDirection direction = SpellDirection::Right;
    GridCoord cast_origin;
    std::vector<GridCoord> matched_cells;
    bool static_fire_bolt = false;
};

struct SpellRuntimeContext {
    GameState* state = nullptr;
    EventQueue* event_queue = nullptr;
    std::function<float(const std::string&)> get_effect_duration_seconds;
};

using SpellRuntimeHandler = std::function<bool(const SpellRuntimeMatch&, const SpellRuntimeContext&)>;

class SpellRuntimeRegistry {
  public:
    void Register(const std::string& spell_name, SpellRuntimeHandler handler);
    bool Cast(const SpellRuntimeMatch& match, const SpellRuntimeContext& context) const;

  private:
    std::unordered_map<std::string, SpellRuntimeHandler> handlers_;
};
