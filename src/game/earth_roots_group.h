#pragma once

#include "game/game_types.h"

enum class EarthRootsGroupState {
    Born = 0,
    Idle = 1,
    Dying = 2,
};

struct EarthRootsGroup {
    int id = -1;
    int owner_player_id = -1;
    int owner_team = 0;
    GridCoord center_cell;
    EarthRootsGroupState state = EarthRootsGroupState::Born;
    float state_time = 0.0f;
    float state_duration = 0.0f;
    float idle_lifetime_remaining_seconds = 0.0f;
    bool active_for_gameplay = false;
    bool alive = true;
};
