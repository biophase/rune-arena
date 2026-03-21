#pragma once

#include "game/game_types.h"

struct FireStormCast {
    int id = -1;
    int owner_player_id = -1;
    int owner_team = 0;
    GridCoord center_cell;
    float elapsed_seconds = 0.0f;
    float duration_seconds = 0.0f;
    bool alive = true;
};
