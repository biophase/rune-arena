#pragma once

#include "game/game_types.h"

struct Rune {
    int id = -1;
    int owner_player_id = -1;
    int owner_team = 0;
    GridCoord cell;
    RuneType rune_type = RuneType::Fire;
    int placement_order = 0;
    bool active = false;
    bool volatile_cast = false;
    float activation_total_seconds = 0.0f;
    float activation_remaining_seconds = 0.0f;
    bool creates_influence_zone = true;
};
