#include "spells/fire_bolt_spell.h"

#include "core/constants.h"

namespace {

Vector2 DirectionToVelocity(SpellDirection direction) {
    switch (direction) {
        case SpellDirection::Left:
            return {-Constants::kProjectileSpeed, 0.0f};
        case SpellDirection::Top:
            return {0.0f, -Constants::kProjectileSpeed};
        case SpellDirection::Bottom:
            return {0.0f, Constants::kProjectileSpeed};
        case SpellDirection::Right:
        default:
            return {Constants::kProjectileSpeed, 0.0f};
    }
}

}  // namespace

std::string FireBoltSpell::GetName() const { return "fire_bolt"; }

void FireBoltSpell::Cast(GameState& state, EventQueue& event_queue) {
    int caster_team = 0;
    for (const auto& player : state.players) {
        if (player.id == caster_player_id_) {
            caster_team = player.team;
            break;
        }
    }

    Projectile projectile;
    projectile.id = state.next_entity_id++;
    projectile.owner_player_id = caster_player_id_;
    projectile.owner_team = caster_team;
    projectile.pos = cast_position_;
    projectile.vel = DirectionToVelocity(direction_);
    projectile.radius = Constants::kProjectileRadius;
    projectile.damage = Constants::kProjectileDamage;
    projectile.animation_key = "projectile_fire_bolt";
    projectile.emitter_enabled = true;
    projectile.emitter_emit_every_frames = Constants::kProjectileSmokeEmitEveryFrames;
    projectile.emitter_frame_counter = 0;
    projectile.alive = true;

    state.projectiles.push_back(projectile);
    event_queue.Push(SpellCastEvent{caster_player_id_, GetName(), cast_position_, direction_});
}
