#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

struct ClientInputMessage {
    int player_id = -1;
    int tick = 0;
    float move_x = 0.0f;
    float move_y = 0.0f;
    float aim_x = 0.0f;
    float aim_y = 0.0f;
    bool primary_pressed = false;
    bool select_fire = false;
    bool select_water = false;
    int seq = 0;
};

struct ClientMoveMessage {
    int player_id = -1;
    int seq = 0;
    int tick = 0;
    int last_received_snapshot_id = 0;
    float move_x = 0.0f;
    float move_y = 0.0f;
    float aim_x = 0.0f;
    float aim_y = 0.0f;
};

struct ClientActionMessage {
    int player_id = -1;
    int seq = 0;
    int last_received_snapshot_id = 0;
    bool primary_pressed = false;
    bool select_fire = false;
    bool select_water = false;
};

struct PlayerSnapshot {
    int id = -1;
    int team = 0;
    float pos_x = 0.0f;
    float pos_y = 0.0f;
    float vel_x = 0.0f;
    float vel_y = 0.0f;
    float aim_dir_x = 1.0f;
    float aim_dir_y = 0.0f;
    int hp = 100;
    int kills = 0;
    bool alive = true;
    int facing = 0;
    int action_state = 0;
    float melee_active_remaining = 0.0f;
    bool rune_placing_mode = false;
    int selected_rune_type = 0;
    float rune_place_cooldown_remaining = 0.0f;
    bool awaiting_respawn = false;
    float respawn_remaining = 0.0f;
    int last_processed_move_seq = 0;
};

struct RuneSnapshot {
    int id = -1;
    int owner_player_id = -1;
    int owner_team = 0;
    int x = 0;
    int y = 0;
    int rune_type = 0;
    int placement_order = 0;
    bool active = true;
};

struct ProjectileSnapshot {
    int id = -1;
    int owner_player_id = -1;
    int owner_team = 0;
    float pos_x = 0.0f;
    float pos_y = 0.0f;
    float vel_x = 0.0f;
    float vel_y = 0.0f;
    float radius = 0.0f;
    int damage = 0;
    std::string animation_key;
    bool emitter_enabled = false;
    int emitter_emit_every_frames = 0;
    int emitter_frame_counter = 0;
    bool alive = true;
};

struct IceWallSnapshot {
    int id = -1;
    int owner_player_id = -1;
    int owner_team = 0;
    int cell_x = 0;
    int cell_y = 0;
    int state = 0;
    float state_time = 0.0f;
    float hp = 0.0f;
    bool alive = true;
};

struct MapObjectSnapshot {
    int id = -1;
    std::string prototype_id;
    int cell_x = 0;
    int cell_y = 0;
    int object_type = 0;
    int hp = 0;
    int state = 0;
    float state_time = 0.0f;
    float death_duration = 0.0f;
    bool collision_enabled = false;
    bool alive = true;
};

struct ServerSnapshotMessage {
    int server_tick = 0;
    int snapshot_id = 0;
    int base_snapshot_id = 0;
    bool is_delta = false;
    float time_remaining = 0.0f;
    float shrink_tiles_per_second = 0.0f;
    float min_arena_radius_tiles = 0.0f;
    float arena_radius_tiles = 0.0f;
    float arena_radius_world = 0.0f;
    float arena_center_world_x = 0.0f;
    float arena_center_world_y = 0.0f;
    bool match_running = false;
    bool match_finished = false;
    int red_team_kills = 0;
    int blue_team_kills = 0;

    std::vector<PlayerSnapshot> players;
    std::vector<int> removed_player_ids;
    std::vector<RuneSnapshot> runes;
    std::vector<int> removed_rune_ids;
    std::vector<ProjectileSnapshot> projectiles;
    std::vector<int> removed_projectile_ids;
    std::vector<IceWallSnapshot> ice_walls;
    std::vector<int> removed_ice_wall_ids;
    std::vector<MapObjectSnapshot> map_objects;
    std::vector<int> removed_map_object_ids;
};

struct LobbyPlayerInfo {
    int player_id = -1;
    std::string name;
};

struct LobbyStateMessage {
    std::vector<LobbyPlayerInfo> players;
    bool host_can_start = false;
    float shrink_tiles_per_second = 0.0f;
    float min_arena_radius_tiles = 0.0f;
};

struct MatchStartMessage {
    bool start = true;
};

nlohmann::json ToJson(const ClientInputMessage& message);
std::optional<ClientInputMessage> ClientInputFromJson(const nlohmann::json& json);
nlohmann::json ToJson(const ClientMoveMessage& message);
std::optional<ClientMoveMessage> ClientMoveFromJson(const nlohmann::json& json);
nlohmann::json ToJson(const ClientActionMessage& message);
std::optional<ClientActionMessage> ClientActionFromJson(const nlohmann::json& json);

nlohmann::json ToJson(const ServerSnapshotMessage& message);
std::optional<ServerSnapshotMessage> ServerSnapshotFromJson(const nlohmann::json& json);

nlohmann::json ToJson(const LobbyStateMessage& message);
std::optional<LobbyStateMessage> LobbyStateFromJson(const nlohmann::json& json);

nlohmann::json ToJson(const MatchStartMessage& message);
std::optional<MatchStartMessage> MatchStartFromJson(const nlohmann::json& json);
