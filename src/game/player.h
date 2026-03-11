#pragma once

#include <string>
#include <vector>

#include <raylib.h>

#include "core/constants.h"
#include "game/game_types.h"

struct Player {
    int id = -1;
    std::string name;
    int team = 0;

    Vector2 pos = {0.0f, 0.0f};
    Vector2 vel = {0.0f, 0.0f};
    Vector2 aim_dir = {1.0f, 0.0f};

    FacingDirection facing = FacingDirection::Right;
    PlayerActionState action_state = PlayerActionState::Idle;

    float radius = Constants::kPlayerRadius;

    int hp = Constants::kMaxHp;
    int kills = 0;
    bool alive = true;
    bool awaiting_respawn = false;
    float respawn_remaining = 0.0f;
    GridCoord spawn_cell = {0, 0};
    float outside_zone_damage_accumulator = 0.0f;

    bool rune_placing_mode = false;
    RuneType selected_rune_type = RuneType::Fire;
    float rune_place_cooldown_duration = Constants::kRunePlaceCooldownSeconds;
    float rune_place_cooldown_remaining = 0.0f;

    float melee_cooldown_remaining = 0.0f;
    float melee_active_remaining = 0.0f;
    std::vector<int> melee_hit_target_ids;

    // Emitter-capable state (currently not active on players).
    bool emitter_enabled = false;
    int emitter_emit_every_frames = 6;
    int emitter_frame_counter = 0;

    int last_input_tick = 0;
    int last_processed_move_seq = 0;
};
