#pragma once

#include "game/game_types.h"

enum class EarthRuneTrapState {
    IdleRune = 0,
    Slamming = 1,
    RootedIdle = 2,
    RootedDeath = 3,
};

enum class FireStormRuneVisualState {
    None = 0,
    Born = 1,
    Idle = 2,
    Dying = 3,
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
    int fire_storm_original_owner_player_id = -1;
    int fire_storm_original_owner_team = 0;
    RuneType fire_storm_original_rune_type = RuneType::None;
    bool fire_storm_temporary = false;
    bool fire_storm_source_rune = false;
    float fire_storm_remaining_seconds = 0.0f;
    FireStormRuneVisualState fire_storm_visual_state = FireStormRuneVisualState::None;
    float fire_storm_visual_state_time = 0.0f;
    float fire_storm_visual_state_duration = 0.0f;
    bool fire_storm_revert_after_death = false;
    bool fire_storm_pending_removal = false;
    bool castle_charging = false;
    int castle_id = -1;
    float castle_charge_elapsed_seconds = 0.0f;
};
