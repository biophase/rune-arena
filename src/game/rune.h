#pragma once

#include "game/game_types.h"

struct Rune {
    int owner_player_id = -1;
    int owner_team = 0;
    GridCoord cell;
    RuneType rune_type = RuneType::Fire;
    int placement_order = 0;
    bool active = true;
};
