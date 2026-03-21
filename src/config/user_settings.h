#pragma once

#include <string>

#include "core/constants.h"
#include "game/game_types.h"

struct UserSettings {
    std::string player_name = "Player";
    int window_width = 1280;
    int window_height = 720;
    bool fullscreen = true;
    bool show_network_debug_panel = true;
    float lobby_shrink_tiles_per_second = Constants::kDefaultShrinkTilesPerSecond;
    float lobby_min_arena_radius_tiles = Constants::kDefaultMinArenaRadiusTiles;
    int lobby_mode_type = static_cast<int>(MatchModeType::MostKillsTimed);
    int lobby_match_length_seconds = Constants::kDefaultMatchDurationSeconds;
    int lobby_best_of_target_kills = Constants::kDefaultBestOfTargetKills;
    float lobby_shrink_start_seconds = Constants::kDefaultShrinkStartSeconds;
};
