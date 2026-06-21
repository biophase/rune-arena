#pragma once

#include <string>
#include <vector>

#include <raylib.h>

struct LightningEffect {
    int id = -1;
    Vector2 start = {0.0f, 0.0f};
    Vector2 end = {0.0f, 0.0f};

    float idle_duration = 0.3f;
    float elapsed = 0.0f;
    float born_elapsed = 0.0f;
    float born_duration = 0.0f;
    bool birthing = false;

    bool dying = false;
    float death_elapsed = 0.0f;
    float death_duration = 0.25f;

    int segment_count = 1;
    std::vector<float> segment_phase_offsets_seconds;
    bool volatile_variant = false;
    std::string born_animation_key;
    std::string idle_animation_key;
    std::string death_animation_key;
    bool has_sort_y_override = false;
    float sort_y_override = 0.0f;

    bool alive = true;
};
