#include "spells/ice_wave_spell.h"

#include <algorithm>
#include <cmath>

#include "core/constants.h"
#include "game/projectile.h"

namespace {

Vector2 BaseDirectionVector(SpellDirection direction) {
    switch (direction) {
        case SpellDirection::Left:
            return {-1.0f, 0.0f};
        case SpellDirection::Top:
            return {0.0f, -1.0f};
        case SpellDirection::Bottom:
            return {0.0f, 1.0f};
        case SpellDirection::Right:
        default:
            return {1.0f, 0.0f};
    }
}

Vector2 RotateUnitVector(Vector2 direction, float radians) {
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    return {direction.x * c - direction.y * s, direction.x * s + direction.y * c};
}

}  // namespace

std::string IceWaveSpell::GetName() const { return "ice_wave"; }

void IceWaveSpell::Cast(GameState& state, EventQueue& event_queue) {
    int caster_team = 0;
    for (const auto& player : state.players) {
        if (player.id == caster_player_id_) {
            caster_team = player.team;
            break;
        }
    }

    const int shard_count = std::max(1, Constants::kIceWaveShardCount);
    const float total_fan_radians = Constants::kIceWaveFanAngleDegrees * (PI / 180.0f);
    const float start_radians = -0.5f * total_fan_radians;
    const float step_radians = shard_count > 1 ? total_fan_radians / static_cast<float>(shard_count - 1) : 0.0f;
    const Vector2 base_direction = BaseDirectionVector(direction_);
    const float speed = std::max(1.0f, Constants::kIceWaveShardSpeed);
    const float lifetime_seconds =
        (Constants::kIceWaveRangeTiles * static_cast<float>(state.map.cell_size)) / speed;

    for (int i = 0; i < shard_count; ++i) {
        const float angle = start_radians + step_radians * static_cast<float>(i);
        const Vector2 direction = RotateUnitVector(base_direction, angle);

        Projectile projectile;
        projectile.id = state.next_entity_id++;
        projectile.owner_player_id = caster_player_id_;
        projectile.owner_team = caster_team;
        projectile.pos = cast_position_;
        projectile.vel = {direction.x * speed, direction.y * speed};
        projectile.radius = Constants::kIceWaveShardCollisionRadius * Constants::kIceWaveShardScale;
        projectile.damage = Constants::kIceWaveShardDamage;
        projectile.animation_key = "ice_shard_projectile_idle";
        projectile.lifetime_remaining = lifetime_seconds;
        projectile.alive = true;
        state.projectiles.push_back(projectile);
    }

    event_queue.Push(SpellCastEvent{caster_player_id_, GetName(), cast_position_, direction_});
}
