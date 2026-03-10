#pragma once

#include <string>

#include <raylib.h>

struct Particle {
    Vector2 pos = {0.0f, 0.0f};
    Vector2 vel = {0.0f, 0.0f};
    Vector2 acc = {0.0f, 0.0f};
    float drag = 0.0f;

    std::string animation_key = "particle_smoke";
    std::string facing = "default";
    float age_seconds = 0.0f;
    int max_cycles = 1;
    bool alive = true;
};
