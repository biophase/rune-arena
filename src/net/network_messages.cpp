#include "net/network_messages.h"

#include <cmath>

namespace {

float Quantize2(float value) { return std::round(value * 100.0f) / 100.0f; }

}  // namespace

nlohmann::json ToJson(const ClientInputMessage& message) {
    return {
        {"player_id", message.player_id},
        {"tick", message.tick},
        {"move_x", message.move_x},
        {"move_y", message.move_y},
        {"aim_x", message.aim_x},
        {"aim_y", message.aim_y},
        {"primary_pressed", message.primary_pressed},
        {"grappling_pressed", message.grappling_pressed},
        {"request_rune_type", message.request_rune_type},
        {"request_item_id", message.request_item_id},
        {"toggle_inventory_mode", message.toggle_inventory_mode},
        {"seq", message.seq},
    };
}

std::optional<ClientInputMessage> ClientInputFromJson(const nlohmann::json& json) {
    ClientInputMessage out;
    out.player_id = json.value("player_id", -1);
    out.tick = json.value("tick", 0);
    out.move_x = json.value("move_x", 0.0f);
    out.move_y = json.value("move_y", 0.0f);
    out.aim_x = json.value("aim_x", 0.0f);
    out.aim_y = json.value("aim_y", 0.0f);
    out.primary_pressed = json.value("primary_pressed", false);
    out.grappling_pressed = json.value("grappling_pressed", false);
    out.request_rune_type = json.value("request_rune_type", -1);
    out.request_item_id = json.value("request_item_id", std::string{});
    out.toggle_inventory_mode = json.value("toggle_inventory_mode", false);
    out.seq = json.value("seq", 0);
    return out;
}

nlohmann::json ToJson(const ClientMoveMessage& message) {
    return {
        {"player_id", message.player_id}, {"seq", message.seq}, {"tick", message.tick},
        {"last_received_snapshot_id", message.last_received_snapshot_id},
        {"move_x", Quantize2(message.move_x)}, {"move_y", Quantize2(message.move_y)},
        {"aim_x", Quantize2(message.aim_x)},   {"aim_y", Quantize2(message.aim_y)},
    };
}

std::optional<ClientMoveMessage> ClientMoveFromJson(const nlohmann::json& json) {
    ClientMoveMessage out;
    out.player_id = json.value("player_id", -1);
    out.seq = json.value("seq", 0);
    out.tick = json.value("tick", 0);
    out.last_received_snapshot_id = json.value("last_received_snapshot_id", 0);
    out.move_x = json.value("move_x", 0.0f);
    out.move_y = json.value("move_y", 0.0f);
    out.aim_x = json.value("aim_x", 0.0f);
    out.aim_y = json.value("aim_y", 0.0f);
    return out;
}

nlohmann::json ToJson(const ClientActionMessage& message) {
    return {
        {"player_id", message.player_id},
        {"seq", message.seq},
        {"last_received_snapshot_id", message.last_received_snapshot_id},
        {"primary_pressed", message.primary_pressed},
        {"grappling_pressed", message.grappling_pressed},
        {"request_rune_type", message.request_rune_type},
        {"request_item_id", message.request_item_id},
        {"toggle_inventory_mode", message.toggle_inventory_mode},
    };
}

std::optional<ClientActionMessage> ClientActionFromJson(const nlohmann::json& json) {
    ClientActionMessage out;
    out.player_id = json.value("player_id", -1);
    out.seq = json.value("seq", 0);
    out.last_received_snapshot_id = json.value("last_received_snapshot_id", 0);
    out.primary_pressed = json.value("primary_pressed", false);
    out.grappling_pressed = json.value("grappling_pressed", false);
    out.request_rune_type = json.value("request_rune_type", -1);
    out.request_item_id = json.value("request_item_id", std::string{});
    out.toggle_inventory_mode = json.value("toggle_inventory_mode", false);
    return out;
}

nlohmann::json ToJson(const ServerSnapshotMessage& message) {
    nlohmann::json out;
    out["server_tick"] = message.server_tick;
    out["snapshot_id"] = message.snapshot_id;
    out["base_snapshot_id"] = message.base_snapshot_id;
    out["is_delta"] = message.is_delta;
    out["time_remaining"] = message.time_remaining;
    out["shrink_tiles_per_second"] = message.shrink_tiles_per_second;
    out["min_arena_radius_tiles"] = message.min_arena_radius_tiles;
    out["arena_radius_tiles"] = message.arena_radius_tiles;
    out["arena_radius_world"] = message.arena_radius_world;
    out["arena_center_world_x"] = message.arena_center_world_x;
    out["arena_center_world_y"] = message.arena_center_world_y;
    out["match_running"] = message.match_running;
    out["match_finished"] = message.match_finished;
    out["red_team_kills"] = message.red_team_kills;
    out["blue_team_kills"] = message.blue_team_kills;

    out["players"] = nlohmann::json::array();
    for (const auto& player : message.players) {
        out["players"].push_back({
            {"id", player.id},
            {"team", player.team},
            {"pos_x", Quantize2(player.pos_x)},
            {"pos_y", Quantize2(player.pos_y)},
            {"vel_x", Quantize2(player.vel_x)},
            {"vel_y", Quantize2(player.vel_y)},
            {"aim_dir_x", Quantize2(player.aim_dir_x)},
            {"aim_dir_y", Quantize2(player.aim_dir_y)},
            {"hp", player.hp},
            {"kills", player.kills},
            {"alive", player.alive},
            {"facing", player.facing},
            {"action_state", player.action_state},
            {"melee_active_remaining", player.melee_active_remaining},
            {"rune_placing_mode", player.rune_placing_mode},
            {"selected_rune_type", player.selected_rune_type},
            {"mana", Quantize2(player.mana)},
            {"max_mana", Quantize2(player.max_mana)},
            {"grappling_cooldown_remaining", Quantize2(player.grappling_cooldown_remaining)},
            {"grappling_cooldown_total", Quantize2(player.grappling_cooldown_total)},
            {"rune_cooldown_remaining", player.rune_cooldown_remaining},
            {"rune_cooldown_total", player.rune_cooldown_total},
            {"rune_charge_counts", player.rune_charge_counts},
            {"status_effects", nlohmann::json::array()},
            {"item_slots", player.item_slots},
            {"item_slot_counts", player.item_slot_counts},
            {"item_slot_cooldown_remaining", player.item_slot_cooldown_remaining},
            {"item_slot_cooldown_total", player.item_slot_cooldown_total},
            {"awaiting_respawn", player.awaiting_respawn},
            {"respawn_remaining", player.respawn_remaining},
            {"last_processed_move_seq", player.last_processed_move_seq},
        });
        auto& status_array = out["players"].back()["status_effects"];
        for (const auto& status : player.status_effects) {
            status_array.push_back({
                {"type", status.type},
                {"remaining_seconds", Quantize2(status.remaining_seconds)},
                {"total_seconds", Quantize2(status.total_seconds)},
                {"magnitude_per_second", Quantize2(status.magnitude_per_second)},
                {"visible", status.visible},
                {"is_buff", status.is_buff},
                {"source_id", status.source_id},
                {"progress", Quantize2(status.progress)},
                {"source_elapsed_seconds", Quantize2(status.source_elapsed_seconds)},
                {"burn_duration_seconds", Quantize2(status.burn_duration_seconds)},
                {"movement_speed_multiplier", Quantize2(status.movement_speed_multiplier)},
                {"source_active", status.source_active},
                {"composite_effect_id", status.composite_effect_id},
            });
        }
    }

    out["runes"] = nlohmann::json::array();
    for (const auto& rune : message.runes) {
        out["runes"].push_back({
            {"id", rune.id},
            {"owner_player_id", rune.owner_player_id},
            {"owner_team", rune.owner_team},
            {"x", rune.x},
            {"y", rune.y},
            {"rune_type", rune.rune_type},
            {"placement_order", rune.placement_order},
            {"active", rune.active},
            {"volatile_cast", rune.volatile_cast},
            {"activation_total_seconds", Quantize2(rune.activation_total_seconds)},
            {"activation_remaining_seconds", Quantize2(rune.activation_remaining_seconds)},
            {"creates_influence_zone", rune.creates_influence_zone},
            {"earth_trap_state", rune.earth_trap_state},
            {"earth_state_time", Quantize2(rune.earth_state_time)},
            {"earth_state_duration", Quantize2(rune.earth_state_duration)},
            {"earth_roots_spawned", rune.earth_roots_spawned},
            {"earth_roots_group_id", rune.earth_roots_group_id},
        });
    }

    out["projectiles"] = nlohmann::json::array();
    for (const auto& projectile : message.projectiles) {
        out["projectiles"].push_back({
            {"id", projectile.id},
            {"owner_player_id", projectile.owner_player_id},
            {"owner_team", projectile.owner_team},
            {"pos_x", Quantize2(projectile.pos_x)},
            {"pos_y", Quantize2(projectile.pos_y)},
            {"vel_x", Quantize2(projectile.vel_x)},
            {"vel_y", Quantize2(projectile.vel_y)},
            {"radius", Quantize2(projectile.radius)},
            {"damage", projectile.damage},
            {"animation_key", projectile.animation_key},
            {"emitter_enabled", projectile.emitter_enabled},
            {"emitter_emit_every_frames", projectile.emitter_emit_every_frames},
            {"emitter_frame_counter", projectile.emitter_frame_counter},
            {"alive", projectile.alive},
        });
    }

    out["ice_walls"] = nlohmann::json::array();
    for (const auto& wall : message.ice_walls) {
        out["ice_walls"].push_back({
            {"id", wall.id},
            {"owner_player_id", wall.owner_player_id},
            {"owner_team", wall.owner_team},
            {"cell_x", wall.cell_x},
            {"cell_y", wall.cell_y},
            {"state", wall.state},
            {"state_time", Quantize2(wall.state_time)},
            {"hp", Quantize2(wall.hp)},
            {"alive", wall.alive},
        });
    }

    out["map_objects"] = nlohmann::json::array();
    for (const auto& object : message.map_objects) {
        out["map_objects"].push_back({
            {"id", object.id},
            {"prototype_id", object.prototype_id},
            {"cell_x", object.cell_x},
            {"cell_y", object.cell_y},
            {"object_type", object.object_type},
            {"hp", object.hp},
            {"state", object.state},
            {"state_time", Quantize2(object.state_time)},
            {"death_duration", Quantize2(object.death_duration)},
            {"collision_enabled", object.collision_enabled},
            {"alive", object.alive},
        });
    }

    out["fire_storm_dummies"] = nlohmann::json::array();
    for (const auto& dummy : message.fire_storm_dummies) {
        out["fire_storm_dummies"].push_back({
            {"id", dummy.id},
            {"owner_player_id", dummy.owner_player_id},
            {"owner_team", dummy.owner_team},
            {"cell_x", dummy.cell_x},
            {"cell_y", dummy.cell_y},
            {"state", dummy.state},
            {"state_time", Quantize2(dummy.state_time)},
            {"state_duration", Quantize2(dummy.state_duration)},
            {"idle_lifetime_remaining_seconds", Quantize2(dummy.idle_lifetime_remaining_seconds)},
            {"alive", dummy.alive},
        });
    }
    out["fire_storm_casts"] = nlohmann::json::array();
    for (const auto& cast : message.fire_storm_casts) {
        out["fire_storm_casts"].push_back({
            {"id", cast.id},
            {"owner_player_id", cast.owner_player_id},
            {"owner_team", cast.owner_team},
            {"center_cell_x", cast.center_cell_x},
            {"center_cell_y", cast.center_cell_y},
            {"elapsed_seconds", Quantize2(cast.elapsed_seconds)},
            {"duration_seconds", Quantize2(cast.duration_seconds)},
            {"alive", cast.alive},
        });
    }
    out["earth_roots_groups"] = nlohmann::json::array();
    for (const auto& group : message.earth_roots_groups) {
        out["earth_roots_groups"].push_back({
            {"id", group.id},
            {"owner_player_id", group.owner_player_id},
            {"owner_team", group.owner_team},
            {"center_cell_x", group.center_cell_x},
            {"center_cell_y", group.center_cell_y},
            {"state", group.state},
            {"state_time", Quantize2(group.state_time)},
            {"state_duration", Quantize2(group.state_duration)},
            {"idle_lifetime_remaining_seconds", Quantize2(group.idle_lifetime_remaining_seconds)},
            {"active_for_gameplay", group.active_for_gameplay},
            {"alive", group.alive},
        });
    }
    out["grappling_hooks"] = nlohmann::json::array();
    for (const auto& hook : message.grappling_hooks) {
        out["grappling_hooks"].push_back({
            {"id", hook.id},
            {"owner_player_id", hook.owner_player_id},
            {"owner_team", hook.owner_team},
            {"head_pos_x", Quantize2(hook.head_pos_x)},
            {"head_pos_y", Quantize2(hook.head_pos_y)},
            {"target_pos_x", Quantize2(hook.target_pos_x)},
            {"target_pos_y", Quantize2(hook.target_pos_y)},
            {"latch_point_x", Quantize2(hook.latch_point_x)},
            {"latch_point_y", Quantize2(hook.latch_point_y)},
            {"pull_destination_x", Quantize2(hook.pull_destination_x)},
            {"pull_destination_y", Quantize2(hook.pull_destination_y)},
            {"phase", hook.phase},
            {"latch_target_type", hook.latch_target_type},
            {"latch_target_id", hook.latch_target_id},
            {"latch_cell_x", hook.latch_cell_x},
            {"latch_cell_y", hook.latch_cell_y},
            {"latched", hook.latched},
            {"animation_time", Quantize2(hook.animation_time)},
            {"pull_elapsed_seconds", Quantize2(hook.pull_elapsed_seconds)},
            {"max_pull_duration_seconds", Quantize2(hook.max_pull_duration_seconds)},
            {"alive", hook.alive},
        });
    }

    out["removed_player_ids"] = message.removed_player_ids;
    out["removed_rune_ids"] = message.removed_rune_ids;
    out["removed_projectile_ids"] = message.removed_projectile_ids;
    out["removed_ice_wall_ids"] = message.removed_ice_wall_ids;
    out["removed_map_object_ids"] = message.removed_map_object_ids;
    out["removed_fire_storm_dummy_ids"] = message.removed_fire_storm_dummy_ids;
    out["removed_fire_storm_cast_ids"] = message.removed_fire_storm_cast_ids;
    out["removed_earth_roots_group_ids"] = message.removed_earth_roots_group_ids;
    out["removed_grappling_hook_ids"] = message.removed_grappling_hook_ids;

    return out;
}

std::optional<ServerSnapshotMessage> ServerSnapshotFromJson(const nlohmann::json& json) {
    ServerSnapshotMessage out;
    out.server_tick = json.value("server_tick", 0);
    out.snapshot_id = json.value("snapshot_id", 0);
    out.base_snapshot_id = json.value("base_snapshot_id", 0);
    out.is_delta = json.value("is_delta", false);
    out.time_remaining = json.value("time_remaining", 0.0f);
    out.shrink_tiles_per_second = json.value("shrink_tiles_per_second", 0.0f);
    out.min_arena_radius_tiles = json.value("min_arena_radius_tiles", 0.0f);
    out.arena_radius_tiles = json.value("arena_radius_tiles", 0.0f);
    out.arena_radius_world = json.value("arena_radius_world", 0.0f);
    out.arena_center_world_x = json.value("arena_center_world_x", 0.0f);
    out.arena_center_world_y = json.value("arena_center_world_y", 0.0f);
    out.match_running = json.value("match_running", false);
    out.match_finished = json.value("match_finished", false);
    out.red_team_kills = json.value("red_team_kills", 0);
    out.blue_team_kills = json.value("blue_team_kills", 0);

    const auto players_it = json.find("players");
    if (players_it != json.end() && players_it->is_array()) {
        for (const auto& item : *players_it) {
            PlayerSnapshot player;
            player.id = item.value("id", -1);
            player.team = item.value("team", 0);
            player.pos_x = item.value("pos_x", 0.0f);
            player.pos_y = item.value("pos_y", 0.0f);
            player.vel_x = item.value("vel_x", 0.0f);
            player.vel_y = item.value("vel_y", 0.0f);
            player.aim_dir_x = item.value("aim_dir_x", 1.0f);
            player.aim_dir_y = item.value("aim_dir_y", 0.0f);
            player.hp = item.value("hp", 100);
            player.kills = item.value("kills", 0);
            player.alive = item.value("alive", true);
            player.facing = item.value("facing", 0);
            player.action_state = item.value("action_state", 0);
            player.melee_active_remaining = item.value("melee_active_remaining", 0.0f);
            player.rune_placing_mode = item.value("rune_placing_mode", false);
            player.selected_rune_type = item.value("selected_rune_type", 0);
            player.mana = item.value("mana", 0.0f);
            player.max_mana = item.value("max_mana", 0.0f);
            player.grappling_cooldown_remaining = item.value("grappling_cooldown_remaining", 0.0f);
            player.grappling_cooldown_total = item.value("grappling_cooldown_total", 0.0f);
            player.rune_cooldown_remaining = item.value("rune_cooldown_remaining", std::vector<float>{});
            player.rune_cooldown_total = item.value("rune_cooldown_total", std::vector<float>{});
            player.rune_charge_counts = item.value("rune_charge_counts", std::vector<int>{});
            const auto status_it = item.find("status_effects");
            if (status_it != item.end() && status_it->is_array()) {
                for (const auto& status_item : *status_it) {
                    PlayerSnapshot::StatusEffectSnapshot status;
                    status.type = status_item.value("type", 0);
                    status.remaining_seconds = status_item.value("remaining_seconds", 0.0f);
                    status.total_seconds = status_item.value("total_seconds", 0.0f);
                    status.magnitude_per_second = status_item.value("magnitude_per_second", 0.0f);
                    status.visible = status_item.value("visible", true);
                    status.is_buff = status_item.value("is_buff", false);
                    status.source_id = status_item.value("source_id", -1);
                    status.progress = status_item.value("progress", 0.0f);
                    status.source_elapsed_seconds = status_item.value("source_elapsed_seconds", 0.0f);
                    status.burn_duration_seconds = status_item.value("burn_duration_seconds", 0.0f);
                    status.movement_speed_multiplier = status_item.value("movement_speed_multiplier", 1.0f);
                    status.source_active = status_item.value("source_active", false);
                    status.composite_effect_id = status_item.value("composite_effect_id", std::string{});
                    player.status_effects.push_back(status);
                }
            }
            player.item_slots = item.value("item_slots", std::vector<std::string>{});
            player.item_slot_counts = item.value("item_slot_counts", std::vector<int>{});
            player.item_slot_cooldown_remaining =
                item.value("item_slot_cooldown_remaining", std::vector<float>{});
            player.item_slot_cooldown_total = item.value("item_slot_cooldown_total", std::vector<float>{});
            player.awaiting_respawn = item.value("awaiting_respawn", false);
            player.respawn_remaining = item.value("respawn_remaining", 0.0f);
            player.last_processed_move_seq = item.value("last_processed_move_seq", 0);
            out.players.push_back(player);
        }
    }

    const auto runes_it = json.find("runes");
    if (runes_it != json.end() && runes_it->is_array()) {
        for (const auto& item : *runes_it) {
            RuneSnapshot rune;
            rune.id = item.value("id", -1);
            rune.owner_player_id = item.value("owner_player_id", -1);
            rune.owner_team = item.value("owner_team", 0);
            rune.x = item.value("x", 0);
            rune.y = item.value("y", 0);
            rune.rune_type = item.value("rune_type", 0);
            rune.placement_order = item.value("placement_order", 0);
            rune.active = item.value("active", false);
            rune.volatile_cast = item.value("volatile_cast", false);
            rune.activation_total_seconds = item.value("activation_total_seconds", 0.0f);
            rune.activation_remaining_seconds = item.value("activation_remaining_seconds", 0.0f);
            rune.creates_influence_zone = item.value("creates_influence_zone", true);
            rune.earth_trap_state = item.value("earth_trap_state", 0);
            rune.earth_state_time = item.value("earth_state_time", 0.0f);
            rune.earth_state_duration = item.value("earth_state_duration", 0.0f);
            rune.earth_roots_spawned = item.value("earth_roots_spawned", false);
            rune.earth_roots_group_id = item.value("earth_roots_group_id", -1);
            out.runes.push_back(rune);
        }
    }

    const auto projectiles_it = json.find("projectiles");
    if (projectiles_it != json.end() && projectiles_it->is_array()) {
        for (const auto& item : *projectiles_it) {
            ProjectileSnapshot projectile;
            projectile.id = item.value("id", -1);
            projectile.owner_player_id = item.value("owner_player_id", -1);
            projectile.owner_team = item.value("owner_team", 0);
            projectile.pos_x = item.value("pos_x", 0.0f);
            projectile.pos_y = item.value("pos_y", 0.0f);
            projectile.vel_x = item.value("vel_x", 0.0f);
            projectile.vel_y = item.value("vel_y", 0.0f);
            projectile.radius = item.value("radius", 0.0f);
            projectile.damage = item.value("damage", 0);
            projectile.animation_key = item.value("animation_key", std::string("projectile_fire_bolt"));
            projectile.emitter_enabled = item.value("emitter_enabled", false);
            projectile.emitter_emit_every_frames = item.value("emitter_emit_every_frames", 0);
            projectile.emitter_frame_counter = item.value("emitter_frame_counter", 0);
            projectile.alive = item.value("alive", true);
            out.projectiles.push_back(projectile);
        }
    }

    const auto walls_it = json.find("ice_walls");
    if (walls_it != json.end() && walls_it->is_array()) {
        for (const auto& item : *walls_it) {
            IceWallSnapshot wall;
            wall.id = item.value("id", -1);
            wall.owner_player_id = item.value("owner_player_id", -1);
            wall.owner_team = item.value("owner_team", 0);
            wall.cell_x = item.value("cell_x", 0);
            wall.cell_y = item.value("cell_y", 0);
            wall.state = item.value("state", 0);
            wall.state_time = item.value("state_time", 0.0f);
            wall.hp = item.value("hp", 0.0f);
            wall.alive = item.value("alive", true);
            out.ice_walls.push_back(wall);
        }
    }

    const auto objects_it = json.find("map_objects");
    if (objects_it != json.end() && objects_it->is_array()) {
        for (const auto& item : *objects_it) {
            MapObjectSnapshot object;
            object.id = item.value("id", -1);
            object.prototype_id = item.value("prototype_id", std::string{});
            object.cell_x = item.value("cell_x", 0);
            object.cell_y = item.value("cell_y", 0);
            object.object_type = item.value("object_type", 0);
            object.hp = item.value("hp", 0);
            object.state = item.value("state", 0);
            object.state_time = item.value("state_time", 0.0f);
            object.death_duration = item.value("death_duration", 0.0f);
            object.collision_enabled = item.value("collision_enabled", false);
            object.alive = item.value("alive", true);
            out.map_objects.push_back(object);
        }
    }

    const auto dummies_it = json.find("fire_storm_dummies");
    if (dummies_it != json.end() && dummies_it->is_array()) {
        for (const auto& item : *dummies_it) {
            FireStormDummySnapshot dummy;
            dummy.id = item.value("id", -1);
            dummy.owner_player_id = item.value("owner_player_id", -1);
            dummy.owner_team = item.value("owner_team", 0);
            dummy.cell_x = item.value("cell_x", 0);
            dummy.cell_y = item.value("cell_y", 0);
            dummy.state = item.value("state", 0);
            dummy.state_time = item.value("state_time", 0.0f);
            dummy.state_duration = item.value("state_duration", 0.0f);
            dummy.idle_lifetime_remaining_seconds = item.value("idle_lifetime_remaining_seconds", -1.0f);
            dummy.alive = item.value("alive", true);
            out.fire_storm_dummies.push_back(dummy);
        }
    }
    const auto casts_it = json.find("fire_storm_casts");
    if (casts_it != json.end() && casts_it->is_array()) {
        for (const auto& item : *casts_it) {
            FireStormCastSnapshot cast;
            cast.id = item.value("id", -1);
            cast.owner_player_id = item.value("owner_player_id", -1);
            cast.owner_team = item.value("owner_team", 0);
            cast.center_cell_x = item.value("center_cell_x", 0);
            cast.center_cell_y = item.value("center_cell_y", 0);
            cast.elapsed_seconds = item.value("elapsed_seconds", 0.0f);
            cast.duration_seconds = item.value("duration_seconds", 0.0f);
            cast.alive = item.value("alive", true);
            out.fire_storm_casts.push_back(cast);
        }
    }
    const auto roots_it = json.find("earth_roots_groups");
    if (roots_it != json.end() && roots_it->is_array()) {
        for (const auto& item : *roots_it) {
            EarthRootsGroupSnapshot group;
            group.id = item.value("id", -1);
            group.owner_player_id = item.value("owner_player_id", -1);
            group.owner_team = item.value("owner_team", 0);
            group.center_cell_x = item.value("center_cell_x", 0);
            group.center_cell_y = item.value("center_cell_y", 0);
            group.state = item.value("state", 0);
            group.state_time = item.value("state_time", 0.0f);
            group.state_duration = item.value("state_duration", 0.0f);
            group.idle_lifetime_remaining_seconds = item.value("idle_lifetime_remaining_seconds", 0.0f);
            group.active_for_gameplay = item.value("active_for_gameplay", false);
            group.alive = item.value("alive", true);
            out.earth_roots_groups.push_back(group);
        }
    }
    const auto hooks_it = json.find("grappling_hooks");
    if (hooks_it != json.end() && hooks_it->is_array()) {
        for (const auto& item : *hooks_it) {
            GrapplingHookSnapshot hook;
            hook.id = item.value("id", -1);
            hook.owner_player_id = item.value("owner_player_id", -1);
            hook.owner_team = item.value("owner_team", 0);
            hook.head_pos_x = item.value("head_pos_x", 0.0f);
            hook.head_pos_y = item.value("head_pos_y", 0.0f);
            hook.target_pos_x = item.value("target_pos_x", 0.0f);
            hook.target_pos_y = item.value("target_pos_y", 0.0f);
            hook.latch_point_x = item.value("latch_point_x", 0.0f);
            hook.latch_point_y = item.value("latch_point_y", 0.0f);
            hook.pull_destination_x = item.value("pull_destination_x", 0.0f);
            hook.pull_destination_y = item.value("pull_destination_y", 0.0f);
            hook.phase = item.value("phase", 0);
            hook.latch_target_type = item.value("latch_target_type", 0);
            hook.latch_target_id = item.value("latch_target_id", -1);
            hook.latch_cell_x = item.value("latch_cell_x", 0);
            hook.latch_cell_y = item.value("latch_cell_y", 0);
            hook.latched = item.value("latched", false);
            hook.animation_time = item.value("animation_time", 0.0f);
            hook.pull_elapsed_seconds = item.value("pull_elapsed_seconds", 0.0f);
            hook.max_pull_duration_seconds = item.value("max_pull_duration_seconds", 0.0f);
            hook.alive = item.value("alive", true);
            out.grappling_hooks.push_back(hook);
        }
    }

    const auto removed_players_it = json.find("removed_player_ids");
    if (removed_players_it != json.end() && removed_players_it->is_array()) {
        for (const auto& item : *removed_players_it) {
            out.removed_player_ids.push_back(item.get<int>());
        }
    }
    const auto removed_runes_it = json.find("removed_rune_ids");
    if (removed_runes_it != json.end() && removed_runes_it->is_array()) {
        for (const auto& item : *removed_runes_it) {
            out.removed_rune_ids.push_back(item.get<int>());
        }
    }
    const auto removed_projectiles_it = json.find("removed_projectile_ids");
    if (removed_projectiles_it != json.end() && removed_projectiles_it->is_array()) {
        for (const auto& item : *removed_projectiles_it) {
            out.removed_projectile_ids.push_back(item.get<int>());
        }
    }
    const auto removed_walls_it = json.find("removed_ice_wall_ids");
    if (removed_walls_it != json.end() && removed_walls_it->is_array()) {
        for (const auto& item : *removed_walls_it) {
            out.removed_ice_wall_ids.push_back(item.get<int>());
        }
    }
    const auto removed_objects_it = json.find("removed_map_object_ids");
    if (removed_objects_it != json.end() && removed_objects_it->is_array()) {
        for (const auto& item : *removed_objects_it) {
            out.removed_map_object_ids.push_back(item.get<int>());
        }
    }
    const auto removed_dummies_it = json.find("removed_fire_storm_dummy_ids");
    if (removed_dummies_it != json.end() && removed_dummies_it->is_array()) {
        for (const auto& item : *removed_dummies_it) {
            out.removed_fire_storm_dummy_ids.push_back(item.get<int>());
        }
    }
    const auto removed_casts_it = json.find("removed_fire_storm_cast_ids");
    if (removed_casts_it != json.end() && removed_casts_it->is_array()) {
        for (const auto& item : *removed_casts_it) {
            out.removed_fire_storm_cast_ids.push_back(item.get<int>());
        }
    }
    const auto removed_roots_it = json.find("removed_earth_roots_group_ids");
    if (removed_roots_it != json.end() && removed_roots_it->is_array()) {
        for (const auto& item : *removed_roots_it) {
            out.removed_earth_roots_group_ids.push_back(item.get<int>());
        }
    }
    const auto removed_hooks_it = json.find("removed_grappling_hook_ids");
    if (removed_hooks_it != json.end() && removed_hooks_it->is_array()) {
        for (const auto& item : *removed_hooks_it) {
            out.removed_grappling_hook_ids.push_back(item.get<int>());
        }
    }

    return out;
}

nlohmann::json ToJson(const LobbyStateMessage& message) {
    nlohmann::json out;
    out["host_can_start"] = message.host_can_start;
    out["mode_type"] = message.mode_type;
    out["round_time_seconds"] = message.round_time_seconds;
    out["best_of_target_kills"] = message.best_of_target_kills;
    out["shrink_tiles_per_second"] = message.shrink_tiles_per_second;
    out["shrink_start_seconds"] = message.shrink_start_seconds;
    out["min_arena_radius_tiles"] = message.min_arena_radius_tiles;
    out["players"] = nlohmann::json::array();
    for (const auto& player : message.players) {
        out["players"].push_back({{"player_id", player.player_id}, {"name", player.name}});
    }
    return out;
}

std::optional<LobbyStateMessage> LobbyStateFromJson(const nlohmann::json& json) {
    LobbyStateMessage out;
    out.host_can_start = json.value("host_can_start", false);
    out.mode_type = json.value("mode_type", 0);
    out.round_time_seconds = json.value("round_time_seconds", 0);
    out.best_of_target_kills = json.value("best_of_target_kills", 0);
    out.shrink_tiles_per_second = json.value("shrink_tiles_per_second", 0.0f);
    out.shrink_start_seconds = json.value("shrink_start_seconds", 0.0f);
    out.min_arena_radius_tiles = json.value("min_arena_radius_tiles", 0.0f);

    const auto players_it = json.find("players");
    if (players_it != json.end() && players_it->is_array()) {
        for (const auto& item : *players_it) {
            LobbyPlayerInfo info;
            info.player_id = item.value("player_id", -1);
            info.name = item.value("name", std::string{});
            out.players.push_back(info);
        }
    }

    return out;
}

nlohmann::json ToJson(const MatchStartMessage& message) { return {{"start", message.start}}; }

std::optional<MatchStartMessage> MatchStartFromJson(const nlohmann::json& json) {
    MatchStartMessage out;
    out.start = json.value("start", true);
    return out;
}
