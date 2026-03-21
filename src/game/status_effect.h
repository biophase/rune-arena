#pragma once

#include <string>

enum class StatusEffectType {
    Regeneration = 0,
    Stunned = 1,
    Rooted = 2,
    RootedRecovery = 3,
};

struct StatusEffectInstance {
    StatusEffectType type = StatusEffectType::Regeneration;
    float remaining_seconds = 0.0f;
    float total_seconds = 0.0f;
    float magnitude_per_second = 0.0f;
    float accumulated_magnitude = 0.0f;
    bool visible = true;
    bool is_buff = false;
    int source_id = -1;
    float progress = 0.0f;
    float source_elapsed_seconds = 0.0f;
    float burn_duration_seconds = 0.0f;
    float movement_speed_multiplier = 1.0f;
    bool source_active = false;
    std::string composite_effect_id;
};
