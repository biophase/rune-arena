#include "emitters/smoke_emitter.h"

#include <random>

#include <raymath.h>

#include "core/constants.h"

void SmokeEmitter::UpdateAndEmit(Projectile& projectile, std::vector<Particle>& out_particles) const {
    if (!projectile.alive || !projectile.emitter_enabled || projectile.emitter_emit_every_frames <= 0) {
        return;
    }

    projectile.emitter_frame_counter += 1;
    if (projectile.emitter_frame_counter < projectile.emitter_emit_every_frames) {
        return;
    }

    projectile.emitter_frame_counter = 0;

    static thread_local std::mt19937 rng(std::random_device{}());
    std::normal_distribution<float> offset_dist(0.0f, Constants::kSmokeEmitterOffsetStdDev);
    std::normal_distribution<float> jitter_dist(0.0f, Constants::kSmokeEmitterVelocityJitterStdDev);

    const Vector2 base = Vector2Scale(projectile.vel, Constants::kProjectileSmokeBackVelocityFactor);
    for (int i = 0; i < Constants::kProjectileSmokeParticlesPerBurst; ++i) {
        Particle particle;
        particle.pos = {projectile.pos.x + offset_dist(rng), projectile.pos.y + offset_dist(rng)};
        const Vector2 jitter = {jitter_dist(rng), jitter_dist(rng)};
        particle.vel = Vector2Add(base, jitter);
        particle.acc = {0.0f, 0.0f};
        particle.drag = 5.0f;
        particle.animation_key = "particle_smoke";
        particle.facing = "default";
        particle.max_cycles = 1;
        particle.age_seconds = 0.0f;
        particle.alive = true;
        out_particles.push_back(particle);
    }
}
