#pragma once

#include <optional>

#include <raylib.h>

#include "game/game_types.h"
#include "gameplay/action_intent.h"

enum class NpcHighLevelState {
    Idle,
    MoveToLocation,
    AttackActor,
    ReturnToSpawn,
    DefendArea,
};

struct NpcActorScaffold {
    int id = -1;
    Vector2 spawn_world = {0.0f, 0.0f};
    Vector2 desired_world = {0.0f, 0.0f};
    NpcHighLevelState state = NpcHighLevelState::Idle;
    std::optional<int> target_actor_id;
    std::optional<GridCoord> target_cell;
    float think_interval_seconds = 0.25f;
    float think_cooldown_remaining = 0.0f;
    ActionIntent pending_intent;
};
