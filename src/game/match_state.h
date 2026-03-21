#pragma once

#include <raylib.h>

#include "core/constants.h"

struct MatchState {
    int round_time_seconds = Constants::kDefaultMatchDurationSeconds;
    float time_remaining = static_cast<float>(Constants::kDefaultMatchDurationSeconds);
    MatchModeType mode_type = MatchModeType::MostKillsTimed;
    int best_of_target_kills = Constants::kDefaultBestOfTargetKills;
    float shrink_tiles_per_second = Constants::kDefaultShrinkTilesPerSecond;
    float shrink_start_seconds = Constants::kDefaultShrinkStartSeconds;
    float min_arena_radius_tiles = Constants::kDefaultMinArenaRadiusTiles;
    float arena_radius_tiles = 0.0f;
    float arena_radius_world = 0.0f;
    Vector2 arena_center_world = {0.0f, 0.0f};
    float elapsed_seconds = 0.0f;

    bool match_running = false;
    bool match_finished = false;

    int red_team_kills = 0;
    int blue_team_kills = 0;
};
