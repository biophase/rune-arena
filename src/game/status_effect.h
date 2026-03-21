#pragma once

#include <string>

enum class StatusEffectType {
    Regeneration = 0,
    Stunned = 1,
};

struct StatusEffectInstance {
    StatusEffectType type = StatusEffectType::Regeneration;
    float remaining_seconds = 0.0f;
    float total_seconds = 0.0f;
    float magnitude_per_second = 0.0f;
    float accumulated_magnitude = 0.0f;
    std::string composite_effect_id;
};
