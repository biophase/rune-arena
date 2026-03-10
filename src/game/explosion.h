#pragma once

#include <vector>

#include <raylib.h>

#include "core/constants.h"

struct Explosion {
    int owner_player_id = -1;
    int owner_team = 0;
    Vector2 pos = {0.0f, 0.0f};
    float radius = Constants::kFireBoltExplosionRadius;
    int damage = Constants::kProjectileDamage;
    float age_seconds = 0.0f;
    float duration_seconds = Constants::kFireBoltExplosionFallbackDuration;
    bool alive = true;
    std::vector<int> excluded_target_ids;
    std::vector<int> already_hit_target_ids;
};
