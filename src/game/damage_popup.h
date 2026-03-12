#pragma once

#include <raylib.h>

#include "core/constants.h"

struct DamagePopup {
    Vector2 pos = {0.0f, 0.0f};
    int amount = 0;
    bool is_heal = false;
    float age_seconds = 0.0f;
    float lifetime_seconds = Constants::kDamagePopupLifetimeSeconds;
    float rise_per_second = Constants::kDamagePopupRisePerSecond;
    bool alive = true;
};
