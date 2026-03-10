#include "net/network_messages.h"

nlohmann::json ToJson(const ClientInputMessage& message) {
    return {
        {"player_id", message.player_id},
        {"tick", message.tick},
        {"move_x", message.move_x},
        {"move_y", message.move_y},
        {"aim_x", message.aim_x},
        {"aim_y", message.aim_y},
        {"primary_pressed", message.primary_pressed},
        {"select_fire", message.select_fire},
        {"select_water", message.select_water},
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
    out.select_fire = json.value("select_fire", false);
    out.select_water = json.value("select_water", false);
    return out;
}

nlohmann::json ToJson(const ServerSnapshotMessage& message) {
    nlohmann::json out;
    out["server_tick"] = message.server_tick;
    out["time_remaining"] = message.time_remaining;
    out["match_running"] = message.match_running;
    out["match_finished"] = message.match_finished;
    out["red_team_kills"] = message.red_team_kills;
    out["blue_team_kills"] = message.blue_team_kills;

    out["players"] = nlohmann::json::array();
    for (const auto& player : message.players) {
        out["players"].push_back({
            {"id", player.id},
            {"name", player.name},
            {"team", player.team},
            {"pos_x", player.pos_x},
            {"pos_y", player.pos_y},
            {"vel_x", player.vel_x},
            {"vel_y", player.vel_y},
            {"aim_dir_x", player.aim_dir_x},
            {"aim_dir_y", player.aim_dir_y},
            {"hp", player.hp},
            {"kills", player.kills},
            {"alive", player.alive},
            {"facing", player.facing},
            {"action_state", player.action_state},
            {"rune_placing_mode", player.rune_placing_mode},
            {"selected_rune_type", player.selected_rune_type},
            {"rune_place_cooldown_remaining", player.rune_place_cooldown_remaining},
        });
    }

    out["runes"] = nlohmann::json::array();
    for (const auto& rune : message.runes) {
        out["runes"].push_back({
            {"owner_player_id", rune.owner_player_id},
            {"owner_team", rune.owner_team},
            {"x", rune.x},
            {"y", rune.y},
            {"rune_type", rune.rune_type},
            {"placement_order", rune.placement_order},
            {"active", rune.active},
        });
    }

    out["projectiles"] = nlohmann::json::array();
    for (const auto& projectile : message.projectiles) {
        out["projectiles"].push_back({
            {"owner_player_id", projectile.owner_player_id},
            {"owner_team", projectile.owner_team},
            {"pos_x", projectile.pos_x},
            {"pos_y", projectile.pos_y},
            {"vel_x", projectile.vel_x},
            {"vel_y", projectile.vel_y},
            {"radius", projectile.radius},
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
            {"owner_player_id", wall.owner_player_id},
            {"owner_team", wall.owner_team},
            {"cell_x", wall.cell_x},
            {"cell_y", wall.cell_y},
            {"state", wall.state},
            {"state_time", wall.state_time},
            {"hp", wall.hp},
            {"alive", wall.alive},
        });
    }

    return out;
}

std::optional<ServerSnapshotMessage> ServerSnapshotFromJson(const nlohmann::json& json) {
    ServerSnapshotMessage out;
    out.server_tick = json.value("server_tick", 0);
    out.time_remaining = json.value("time_remaining", 0.0f);
    out.match_running = json.value("match_running", false);
    out.match_finished = json.value("match_finished", false);
    out.red_team_kills = json.value("red_team_kills", 0);
    out.blue_team_kills = json.value("blue_team_kills", 0);

    const auto players_it = json.find("players");
    if (players_it != json.end() && players_it->is_array()) {
        for (const auto& item : *players_it) {
            PlayerSnapshot player;
            player.id = item.value("id", -1);
            player.name = item.value("name", std::string{});
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
            player.rune_placing_mode = item.value("rune_placing_mode", false);
            player.selected_rune_type = item.value("selected_rune_type", 0);
            player.rune_place_cooldown_remaining = item.value("rune_place_cooldown_remaining", 0.0f);
            out.players.push_back(player);
        }
    }

    const auto runes_it = json.find("runes");
    if (runes_it != json.end() && runes_it->is_array()) {
        for (const auto& item : *runes_it) {
            RuneSnapshot rune;
            rune.owner_player_id = item.value("owner_player_id", -1);
            rune.owner_team = item.value("owner_team", 0);
            rune.x = item.value("x", 0);
            rune.y = item.value("y", 0);
            rune.rune_type = item.value("rune_type", 0);
            rune.placement_order = item.value("placement_order", 0);
            rune.active = item.value("active", true);
            out.runes.push_back(rune);
        }
    }

    const auto projectiles_it = json.find("projectiles");
    if (projectiles_it != json.end() && projectiles_it->is_array()) {
        for (const auto& item : *projectiles_it) {
            ProjectileSnapshot projectile;
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

    return out;
}

nlohmann::json ToJson(const LobbyStateMessage& message) {
    nlohmann::json out;
    out["host_can_start"] = message.host_can_start;
    out["players"] = nlohmann::json::array();
    for (const auto& player : message.players) {
        out["players"].push_back({{"player_id", player.player_id}, {"name", player.name}});
    }
    return out;
}

std::optional<LobbyStateMessage> LobbyStateFromJson(const nlohmann::json& json) {
    LobbyStateMessage out;
    out.host_can_start = json.value("host_can_start", false);

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
