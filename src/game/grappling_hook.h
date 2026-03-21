#pragma once

#include <raylib.h>

#include "game/game_types.h"

enum class GrapplingHookPhase {
    Firing = 0,
    Pulling = 1,
    Retracting = 2,
};

enum class GrapplingHookLatchTargetType {
    None = 0,
    MapObject = 1,
    IceWall = 2,
};

struct GrapplingHook {
    int id = -1;
    int owner_player_id = -1;
    int owner_team = 0;

    Vector2 head_pos = {0.0f, 0.0f};
    Vector2 target_pos = {0.0f, 0.0f};
    Vector2 latch_point = {0.0f, 0.0f};
    Vector2 pull_destination = {0.0f, 0.0f};

    GrapplingHookPhase phase = GrapplingHookPhase::Firing;
    GrapplingHookLatchTargetType latch_target_type = GrapplingHookLatchTargetType::None;
    int latch_target_id = -1;
    GridCoord latch_cell = {0, 0};
    bool latched = false;

    float animation_time = 0.0f;
    float pull_elapsed_seconds = 0.0f;
    float max_pull_duration_seconds = 0.0f;
    bool alive = true;
};
