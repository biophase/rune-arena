#pragma once

#include <raylib.h>

struct FireWaveSegment {
    int id = -1;
    int source_spirit_id = -1;
    int owner_player_id = -1;
    int owner_team = 0;
    int segment_index = 0;
    Vector2 origin_world = {0.0f, 0.0f};
    float direction_radians = 0.0f;
    float start_time_seconds = 0.0f;
    float duration_seconds = 0.0f;
    float range_world = 0.0f;
    bool alive = true;
};
