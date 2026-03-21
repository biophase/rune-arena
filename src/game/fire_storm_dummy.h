#pragma once

#include "game/game_types.h"

enum class FireStormDummyState {
    Born = 0,
    Idle = 1,
    Dying = 2,
};

struct FireStormDummy {
    int id = -1;
    int owner_player_id = -1;
    int owner_team = 0;
    GridCoord cell;
    FireStormDummyState state = FireStormDummyState::Born;
    float state_time = 0.0f;
    float state_duration = 0.0f;
    float idle_lifetime_remaining_seconds = -1.0f;
    bool alive = true;
};
