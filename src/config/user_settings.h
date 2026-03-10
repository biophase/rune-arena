#pragma once

#include <string>

struct UserSettings {
    std::string player_name = "Player";
    int window_width = 1280;
    int window_height = 720;
    bool fullscreen = true;
};
