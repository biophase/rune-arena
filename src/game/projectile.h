#pragma once

#include <string>

#include <raylib.h>

#include "core/constants.h"

struct Projectile {
    int id = -1;
    int owner_player_id = -1;
    int owner_team = 0;
    Vector2 pos = {0.0f, 0.0f};
    Vector2 vel = {0.0f, 0.0f};
    Vector2 acc = {0.0f, 0.0f};
    float drag = 0.0f;
    float radius = Constants::kProjectileRadius;
    int damage = Constants::kProjectileDamage;
    std::string animation_key = "projectile_fire_bolt";
    float upgrade_pause_remaining = 0.0f;
    Vector2 resume_vel = {0.0f, 0.0f};

    bool emitter_enabled = false;
    int emitter_emit_every_frames = Constants::kProjectileSmokeEmitEveryFrames;
    int emitter_frame_counter = 0;

    bool alive = true;
};

inline void ConfigureLargeStaticFireBolt(Projectile& projectile, bool with_upgrade_pause) {
    projectile.animation_key = "fire_storm_static_large";
    projectile.radius *= Constants::kStaticFireBoltScaleMultiplier;
    projectile.resume_vel = {projectile.vel.x * Constants::kStaticFireBoltSpeedMultiplier,
                             projectile.vel.y * Constants::kStaticFireBoltSpeedMultiplier};
    if (with_upgrade_pause) {
        projectile.vel = {0.0f, 0.0f};
        projectile.upgrade_pause_remaining = Constants::kStaticFireBoltUpgradePauseSeconds;
    } else {
        projectile.vel = projectile.resume_vel;
        projectile.upgrade_pause_remaining = 0.0f;
    }
}
