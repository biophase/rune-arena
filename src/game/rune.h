#pragma once

#include "game/game_types.h"

enum class EarthRuneTrapState {
    IdleRune = 0,
    Slamming = 1,
    RootedIdle = 2,
    RootedDeath = 3,
};

struct Rune {
    int id = -1;
    int owner_player_id = -1;
    int owner_team = 0;
    GridCoord cell;
    RuneType rune_type = RuneType::Fire;
    int placement_order = 0;
    bool active = false;
    bool volatile_cast = false;
    float activation_total_seconds = 0.0f;
    float activation_remaining_seconds = 0.0f;
    bool creates_influence_zone = true;
    EarthRuneTrapState earth_trap_state = EarthRuneTrapState::IdleRune;
    float earth_state_time = 0.0f;
    float earth_state_duration = 0.0f;
    bool earth_roots_spawned = false;
    int earth_roots_group_id = -1;
};
