#pragma once

#include <vector>

#include <raylib.h>

struct LightningEffect {
    int id = -1;
    Vector2 start = {0.0f, 0.0f};
    Vector2 end = {0.0f, 0.0f};

    float idle_duration = 0.3f;
    float elapsed = 0.0f;

    bool dying = false;
    float death_elapsed = 0.0f;
    float death_duration = 0.25f;

    int segment_count = 1;
    std::vector<float> segment_phase_offsets_seconds;

    bool alive = true;
};
