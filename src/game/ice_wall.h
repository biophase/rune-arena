#pragma once

#include "core/constants.h"
#include "game/game_types.h"

enum class IceWallState {
    Materializing = 0,
    Active = 1,
    Dying = 2,
};

struct IceWallPiece {
    int id = -1;
    GridCoord cell;
    int owner_player_id = -1;
    int owner_team = 0;

    IceWallState state = IceWallState::Materializing;
    float state_time = 0.0f;
    float hp = Constants::kIceWallMaxHp;
    bool alive = true;
};
