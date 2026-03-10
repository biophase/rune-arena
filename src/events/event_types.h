#pragma once

#include <string>
#include <variant>
#include <vector>

#include <raylib.h>

#include "game/game_types.h"

struct RunePlacedEvent {
    int player_id = -1;
    int team = 0;
    GridCoord cell;
    RuneType rune_type = RuneType::Fire;
    int placement_order = 0;
};

struct RunePatternCompletedEvent {
    std::string spell_name;
    SpellDirection direction = SpellDirection::Right;
    GridCoord origin_cell;
    std::vector<GridCoord> matched_cells;
};

struct SpellCastEvent {
    int caster_player_id = -1;
    std::string spell_name;
    Vector2 cast_position = {0.0f, 0.0f};
    SpellDirection direction = SpellDirection::Right;
};

struct PlayerHitEvent {
    int attacker_player_id = -1;
    int target_player_id = -1;
    int damage = 0;
    std::string source;
};

struct PlayerDiedEvent {
    int victim_player_id = -1;
    int killer_player_id = -1;
};

struct MatchEndedEvent {
    int winning_team = -1;
};

using Event = std::variant<RunePlacedEvent, RunePatternCompletedEvent, SpellCastEvent, PlayerHitEvent,
                           PlayerDiedEvent, MatchEndedEvent>;
