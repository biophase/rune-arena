#include "gameplay/snapshot_translation.h"

namespace SnapshotTranslation {

PlayerSnapshot BuildPlayerSnapshot(const Player& player) {
    PlayerSnapshot snapshot;
    snapshot.id = player.id;
    snapshot.team = player.team;
    snapshot.pos_x = player.pos.x;
    snapshot.pos_y = player.pos.y;
    snapshot.vel_x = player.vel.x;
    snapshot.vel_y = player.vel.y;
    snapshot.aim_dir_x = player.aim_dir.x;
    snapshot.aim_dir_y = player.aim_dir.y;
    snapshot.hp = player.hp;
    snapshot.kills = player.kills;
    snapshot.alive = player.alive;
    snapshot.facing = static_cast<int>(player.facing);
    snapshot.action_state = static_cast<int>(player.action_state);
    snapshot.melee_active_remaining = player.melee_active_remaining;
    snapshot.rune_placing_mode = player.rune_placing_mode;
    snapshot.selected_rune_type = static_cast<int>(player.selected_rune_type);
    snapshot.mana = player.mana;
    snapshot.max_mana = player.max_mana;
    snapshot.grappling_cooldown_remaining = player.grappling_cooldown_remaining;
    snapshot.grappling_cooldown_total = player.grappling_cooldown_total;
    snapshot.rune_cooldown_remaining.assign(player.rune_cooldown_remaining.begin(), player.rune_cooldown_remaining.end());
    snapshot.rune_cooldown_total.assign(player.rune_cooldown_total.begin(), player.rune_cooldown_total.end());
    snapshot.rune_charge_counts.assign(player.rune_charge_counts.begin(), player.rune_charge_counts.end());
    snapshot.weapon_slots.assign(player.weapon_slots.begin(), player.weapon_slots.end());
    for (const auto& status : player.status_effects) {
        PlayerSnapshot::StatusEffectSnapshot status_snapshot;
        status_snapshot.type = static_cast<int>(status.type);
        status_snapshot.remaining_seconds = status.remaining_seconds;
        status_snapshot.total_seconds = status.total_seconds;
        status_snapshot.magnitude_per_second = status.magnitude_per_second;
        status_snapshot.visible = status.visible;
        status_snapshot.is_buff = status.is_buff;
        status_snapshot.source_id = status.source_id;
        status_snapshot.progress = status.progress;
        status_snapshot.source_elapsed_seconds = status.source_elapsed_seconds;
        status_snapshot.burn_duration_seconds = status.burn_duration_seconds;
        status_snapshot.movement_speed_multiplier = status.movement_speed_multiplier;
        status_snapshot.source_active = status.source_active;
        status_snapshot.composite_effect_id = status.composite_effect_id;
        snapshot.status_effects.push_back(status_snapshot);
    }
    snapshot.item_slots.assign(player.item_slots.begin(), player.item_slots.end());
    snapshot.item_slot_counts.assign(player.item_slot_counts.begin(), player.item_slot_counts.end());
    snapshot.item_slot_cooldown_remaining.assign(player.item_slot_cooldown_remaining.begin(),
                                                 player.item_slot_cooldown_remaining.end());
    snapshot.item_slot_cooldown_total.assign(player.item_slot_cooldown_total.begin(), player.item_slot_cooldown_total.end());
    snapshot.awaiting_respawn = player.awaiting_respawn;
    snapshot.respawn_remaining = player.respawn_remaining;
    snapshot.last_processed_move_seq = player.last_processed_move_seq;
    return snapshot;
}

void ApplyPlayerSnapshot(Player* player, const PlayerSnapshot& snapshot, const std::string& resolved_name) {
    if (player == nullptr) {
        return;
    }
    player->id = snapshot.id;
    player->name = resolved_name;
    player->team = snapshot.team;
    player->pos = {snapshot.pos_x, snapshot.pos_y};
    player->vel = {snapshot.vel_x, snapshot.vel_y};
    player->aim_dir = {snapshot.aim_dir_x, snapshot.aim_dir_y};
    player->hp = snapshot.hp;
    player->kills = snapshot.kills;
    player->alive = snapshot.alive;
    player->facing = static_cast<FacingDirection>(snapshot.facing);
    player->action_state = static_cast<PlayerActionState>(snapshot.action_state);
    player->melee_active_remaining = snapshot.melee_active_remaining;
    player->rune_placing_mode = snapshot.rune_placing_mode;
    player->selected_rune_type = static_cast<RuneType>(snapshot.selected_rune_type);
    player->mana = snapshot.mana;
    player->max_mana = snapshot.max_mana;
    player->grappling_cooldown_remaining = snapshot.grappling_cooldown_remaining;
    player->grappling_cooldown_total = snapshot.grappling_cooldown_total;
    player->status_effects.clear();
    for (const auto& status_snapshot : snapshot.status_effects) {
        StatusEffectInstance status;
        status.type = static_cast<StatusEffectType>(status_snapshot.type);
        status.remaining_seconds = status_snapshot.remaining_seconds;
        status.total_seconds = status_snapshot.total_seconds;
        status.magnitude_per_second = status_snapshot.magnitude_per_second;
        status.visible = status_snapshot.visible;
        status.is_buff = status_snapshot.is_buff;
        status.source_id = status_snapshot.source_id;
        status.progress = status_snapshot.progress;
        status.source_elapsed_seconds = status_snapshot.source_elapsed_seconds;
        status.burn_duration_seconds = status_snapshot.burn_duration_seconds;
        status.movement_speed_multiplier = status_snapshot.movement_speed_multiplier;
        status.source_active = status_snapshot.source_active;
        status.composite_effect_id = status_snapshot.composite_effect_id;
        status.accumulated_magnitude = 0.0f;
        player->status_effects.push_back(status);
    }
    for (size_t i = 0; i < player->item_slots.size() && i < snapshot.item_slots.size(); ++i) {
        player->item_slots[i] = snapshot.item_slots[i];
    }
    for (size_t i = 0; i < player->item_slot_counts.size() && i < snapshot.item_slot_counts.size(); ++i) {
        player->item_slot_counts[i] = snapshot.item_slot_counts[i];
    }
    for (size_t i = 0; i < player->item_slot_cooldown_remaining.size() && i < snapshot.item_slot_cooldown_remaining.size();
         ++i) {
        player->item_slot_cooldown_remaining[i] = snapshot.item_slot_cooldown_remaining[i];
    }
    for (size_t i = 0; i < player->item_slot_cooldown_total.size() && i < snapshot.item_slot_cooldown_total.size(); ++i) {
        player->item_slot_cooldown_total[i] = snapshot.item_slot_cooldown_total[i];
    }
    player->awaiting_respawn = snapshot.awaiting_respawn;
    player->respawn_remaining = snapshot.respawn_remaining;
    player->last_processed_move_seq = snapshot.last_processed_move_seq;
    for (size_t i = 0; i < player->rune_cooldown_remaining.size() && i < snapshot.rune_cooldown_remaining.size(); ++i) {
        player->rune_cooldown_remaining[i] = snapshot.rune_cooldown_remaining[i];
    }
    for (size_t i = 0; i < player->rune_cooldown_total.size() && i < snapshot.rune_cooldown_total.size(); ++i) {
        player->rune_cooldown_total[i] = snapshot.rune_cooldown_total[i];
    }
    for (size_t i = 0; i < player->rune_charge_counts.size() && i < snapshot.rune_charge_counts.size(); ++i) {
        player->rune_charge_counts[i] = snapshot.rune_charge_counts[i];
    }
    for (size_t i = 0; i < player->weapon_slots.size() && i < snapshot.weapon_slots.size(); ++i) {
        player->weapon_slots[i] = snapshot.weapon_slots[i];
    }
}

RuneSnapshot BuildRuneSnapshot(const Rune& rune) {
    RuneSnapshot snapshot;
    snapshot.id = rune.id;
    snapshot.owner_player_id = rune.owner_player_id;
    snapshot.owner_team = rune.owner_team;
    snapshot.x = rune.cell.x;
    snapshot.y = rune.cell.y;
    snapshot.rune_type = static_cast<int>(rune.rune_type);
    snapshot.placement_order = rune.placement_order;
    snapshot.active = rune.active;
    snapshot.volatile_cast = rune.volatile_cast;
    snapshot.activation_total_seconds = rune.activation_total_seconds;
    snapshot.activation_remaining_seconds = rune.activation_remaining_seconds;
    snapshot.creates_influence_zone = rune.creates_influence_zone;
    snapshot.earth_trap_state = static_cast<int>(rune.earth_trap_state);
    snapshot.earth_state_time = rune.earth_state_time;
    snapshot.earth_state_duration = rune.earth_state_duration;
    snapshot.earth_roots_spawned = rune.earth_roots_spawned;
    snapshot.earth_roots_group_id = rune.earth_roots_group_id;
    snapshot.fire_storm_original_owner_player_id = rune.fire_storm_original_owner_player_id;
    snapshot.fire_storm_original_owner_team = rune.fire_storm_original_owner_team;
    snapshot.fire_storm_original_rune_type = static_cast<int>(rune.fire_storm_original_rune_type);
    snapshot.fire_storm_temporary = rune.fire_storm_temporary;
    snapshot.fire_storm_source_rune = rune.fire_storm_source_rune;
    snapshot.fire_storm_remaining_seconds = rune.fire_storm_remaining_seconds;
    snapshot.fire_storm_visual_state = static_cast<int>(rune.fire_storm_visual_state);
    snapshot.fire_storm_visual_state_time = rune.fire_storm_visual_state_time;
    snapshot.fire_storm_visual_state_duration = rune.fire_storm_visual_state_duration;
    snapshot.fire_storm_revert_after_death = rune.fire_storm_revert_after_death;
    snapshot.fire_storm_pending_removal = rune.fire_storm_pending_removal;
    return snapshot;
}

ProjectileSnapshot BuildProjectileSnapshot(const Projectile& projectile) {
    ProjectileSnapshot snapshot;
    snapshot.id = projectile.id;
    snapshot.owner_player_id = projectile.owner_player_id;
    snapshot.owner_team = projectile.owner_team;
    snapshot.pos_x = projectile.pos.x;
    snapshot.pos_y = projectile.pos.y;
    snapshot.vel_x = projectile.vel.x;
    snapshot.vel_y = projectile.vel.y;
    snapshot.radius = projectile.radius;
    snapshot.damage = projectile.damage;
    snapshot.animation_key = projectile.animation_key;
    snapshot.emitter_enabled = projectile.emitter_enabled;
    snapshot.emitter_emit_every_frames = projectile.emitter_emit_every_frames;
    snapshot.emitter_frame_counter = projectile.emitter_frame_counter;
    snapshot.alive = projectile.alive;
    return snapshot;
}

IceWallSnapshot BuildIceWallSnapshot(const IceWallPiece& wall) {
    IceWallSnapshot snapshot;
    snapshot.id = wall.id;
    snapshot.owner_player_id = wall.owner_player_id;
    snapshot.owner_team = wall.owner_team;
    snapshot.cell_x = wall.cell.x;
    snapshot.cell_y = wall.cell.y;
    snapshot.state = static_cast<int>(wall.state);
    snapshot.state_time = wall.state_time;
    snapshot.hp = wall.hp;
    snapshot.alive = wall.alive;
    return snapshot;
}

MapObjectSnapshot BuildMapObjectSnapshot(const MapObjectInstance& object) {
    MapObjectSnapshot snapshot;
    snapshot.id = object.id;
    snapshot.prototype_id = object.prototype_id;
    snapshot.cell_x = object.cell.x;
    snapshot.cell_y = object.cell.y;
    snapshot.object_type = static_cast<int>(object.type);
    snapshot.hp = object.hp;
    snapshot.state = static_cast<int>(object.state);
    snapshot.state_time = object.state_time;
    snapshot.death_duration = object.death_duration;
    snapshot.collision_enabled = object.collision_enabled;
    snapshot.alive = object.alive;
    return snapshot;
}

FireStormDummySnapshot BuildFireStormDummySnapshot(const FireStormDummy& dummy) {
    FireStormDummySnapshot snapshot;
    snapshot.id = dummy.id;
    snapshot.owner_player_id = dummy.owner_player_id;
    snapshot.owner_team = dummy.owner_team;
    snapshot.cell_x = dummy.cell.x;
    snapshot.cell_y = dummy.cell.y;
    snapshot.state = static_cast<int>(dummy.state);
    snapshot.state_time = dummy.state_time;
    snapshot.state_duration = dummy.state_duration;
    snapshot.idle_lifetime_remaining_seconds = dummy.idle_lifetime_remaining_seconds;
    snapshot.alive = dummy.alive;
    return snapshot;
}

FireStormCastSnapshot BuildFireStormCastSnapshot(const FireStormCast& cast) {
    FireStormCastSnapshot snapshot;
    snapshot.id = cast.id;
    snapshot.owner_player_id = cast.owner_player_id;
    snapshot.owner_team = cast.owner_team;
    snapshot.center_cell_x = cast.center_cell.x;
    snapshot.center_cell_y = cast.center_cell.y;
    snapshot.source_cell_x.reserve(cast.source_cells.size());
    snapshot.source_cell_y.reserve(cast.source_cells.size());
    for (const GridCoord& cell : cast.source_cells) {
        snapshot.source_cell_x.push_back(cell.x);
        snapshot.source_cell_y.push_back(cell.y);
    }
    snapshot.target_cell_x.reserve(cast.target_cells.size());
    snapshot.target_cell_y.reserve(cast.target_cells.size());
    for (const GridCoord& cell : cast.target_cells) {
        snapshot.target_cell_x.push_back(cell.x);
        snapshot.target_cell_y.push_back(cell.y);
    }
    snapshot.elapsed_seconds = cast.elapsed_seconds;
    snapshot.duration_seconds = cast.duration_seconds;
    snapshot.alive = cast.alive;
    return snapshot;
}

EarthRootsGroupSnapshot BuildEarthRootsGroupSnapshot(const EarthRootsGroup& group) {
    EarthRootsGroupSnapshot snapshot;
    snapshot.id = group.id;
    snapshot.owner_player_id = group.owner_player_id;
    snapshot.owner_team = group.owner_team;
    snapshot.center_cell_x = group.center_cell.x;
    snapshot.center_cell_y = group.center_cell.y;
    snapshot.state = static_cast<int>(group.state);
    snapshot.state_time = group.state_time;
    snapshot.state_duration = group.state_duration;
    snapshot.idle_lifetime_remaining_seconds = group.idle_lifetime_remaining_seconds;
    snapshot.active_for_gameplay = group.active_for_gameplay;
    snapshot.alive = group.alive;
    return snapshot;
}

GrapplingHookSnapshot BuildGrapplingHookSnapshot(const GrapplingHook& hook) {
    GrapplingHookSnapshot snapshot;
    snapshot.id = hook.id;
    snapshot.owner_player_id = hook.owner_player_id;
    snapshot.owner_team = hook.owner_team;
    snapshot.head_pos_x = hook.head_pos.x;
    snapshot.head_pos_y = hook.head_pos.y;
    snapshot.target_pos_x = hook.target_pos.x;
    snapshot.target_pos_y = hook.target_pos.y;
    snapshot.latch_point_x = hook.latch_point.x;
    snapshot.latch_point_y = hook.latch_point.y;
    snapshot.pull_destination_x = hook.pull_destination.x;
    snapshot.pull_destination_y = hook.pull_destination.y;
    snapshot.phase = static_cast<int>(hook.phase);
    snapshot.latch_target_type = static_cast<int>(hook.latch_target_type);
    snapshot.latch_target_id = hook.latch_target_id;
    snapshot.latch_cell_x = hook.latch_cell.x;
    snapshot.latch_cell_y = hook.latch_cell.y;
    snapshot.latched = hook.latched;
    snapshot.animation_time = hook.animation_time;
    snapshot.pull_elapsed_seconds = hook.pull_elapsed_seconds;
    snapshot.max_pull_duration_seconds = hook.max_pull_duration_seconds;
    snapshot.alive = hook.alive;
    return snapshot;
}

}  // namespace SnapshotTranslation
