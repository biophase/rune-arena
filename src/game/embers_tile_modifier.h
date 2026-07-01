#pragma once

#include "game/game_types.h"

struct EmbersTileModifier {
    int id = -1;
    int source_spirit_id = -1;
    int owner_player_id = -1;
    int owner_team = 0;
    GridCoord cell;
    float remaining_seconds = 0.0f;
    float total_seconds = 0.0f;
    bool alive = true;
};
