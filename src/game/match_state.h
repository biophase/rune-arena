#pragma once

#include "core/constants.h"

struct MatchState {
    int round_time_seconds = Constants::kMatchDurationSeconds;
    float time_remaining = static_cast<float>(Constants::kMatchDurationSeconds);

    bool match_running = false;
    bool match_finished = false;

    int red_team_kills = 0;
    int blue_team_kills = 0;
};
