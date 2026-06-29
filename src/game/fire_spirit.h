#pragma once

#include <vector>

#include <raylib.h>

enum class FireSpiritState {
    Idle = 0,
    Projectile = 1,
    Exploding = 2,
    Dead = 3,
};

struct FireSpiritTrailSample {
    Vector2 world = {0.0f, 0.0f};
    float age_seconds = 0.0f;
};

struct FireSpirit {
    int id = -1;
    int flower_object_id = -1;
    int owner_player_id = -1;
    int owner_team = 0;

    FireSpiritState state = FireSpiritState::Idle;

    Vector2 pos = {0.0f, 0.0f};
    Vector2 vel = {0.0f, 0.0f};
    Vector2 target_world = {0.0f, 0.0f};
    int spawn_order = 0;
    float age_seconds = 0.0f;

    Vector2 launch_world = {0.0f, 0.0f};
    Vector2 impact_world = {0.0f, 0.0f};
    float launch_time_seconds = 0.0f;
    float impact_time_seconds = 0.0f;
    float travel_duration_seconds = 0.0f;
    float peak_height = 0.0f;
    float projectile_animation_time = 0.0f;

    std::vector<FireSpiritTrailSample> trail_samples;
    float trail_distance_accumulator = 0.0f;
    float spark_distance_accumulator = 0.0f;
    Vector2 last_trail_sample_world = {0.0f, 0.0f};
    bool has_last_trail_sample_world = false;
    float dead_trail_linger_remaining = 0.0f;

    bool alive = true;
};
