#pragma once

#include <array>
#include <string>
#include <vector>

#include <raylib.h>

#include "core/constants.h"
#include "game/game_types.h"
#include "game/status_effect.h"

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
    int selected_rune_slot = 0;
    std::array<RuneType, 4> rune_slots = {RuneType::Fire, RuneType::Water, RuneType::Catalyst, RuneType::Earth};
    std::array<float, 5> rune_cooldown_remaining = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    std::array<float, 5> rune_cooldown_total = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    std::array<int, 5> rune_charge_counts = {0, 0, 0, 0, 0};

    std::array<std::string, 4> item_slots = {"", "", "", ""};
    std::array<int, 4> item_slot_counts = {0, 0, 0, 0};
    std::array<float, 4> item_slot_cooldown_remaining = {0.0f, 0.0f, 0.0f, 0.0f};
    std::array<float, 4> item_slot_cooldown_total = {0.0f, 0.0f, 0.0f, 0.0f};
    std::array<std::string, 2> weapon_slots = {"sword_item", "grappling_hook_item"};

    bool inventory_mode = false;
    bool ui_dragging_slot = false;
    SlotFamily ui_drag_source_family = SlotFamily::Rune;
    int ui_drag_source_index = -1;
    RuneType ui_drag_rune_type = RuneType::None;
    std::string ui_drag_item_id;
    int ui_drag_item_count = 0;
    float ui_drag_item_cooldown_remaining = 0.0f;
    float ui_drag_item_cooldown_total = 0.0f;

    float mana = Constants::kDefaultPlayerMaxMana;
    float max_mana = Constants::kDefaultPlayerMaxMana;
    float mana_regen_per_second = Constants::kDefaultManaRegenPerSecond;
    std::vector<StatusEffectInstance> status_effects;

    float melee_cooldown_remaining = 0.0f;
    float melee_active_remaining = 0.0f;
    float grappling_cooldown_remaining = 0.0f;
    float grappling_cooldown_total = Constants::kGrapplingHookCooldownSeconds;
    std::vector<int> melee_hit_target_ids;
    std::vector<int> melee_hit_object_ids;

    // Emitter-capable state (currently not active on players).
    bool emitter_enabled = false;
    int emitter_emit_every_frames = 6;
    int emitter_frame_counter = 0;

    int last_input_tick = 0;
    int last_processed_move_seq = 0;
};
