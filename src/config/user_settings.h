#pragma once

#include <string>

#include "core/constants.h"

struct UserSettings {
    std::string player_name = "Player";
    int window_width = 1280;
    int window_height = 720;
    bool fullscreen = true;
    float lobby_shrink_tiles_per_second = Constants::kDefaultShrinkTilesPerSecond;
    float lobby_min_arena_radius_tiles = Constants::kDefaultMinArenaRadiusTiles;
};
