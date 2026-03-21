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
    bool grappling_pressed = false;
    int request_rune_type = -1;
    std::string request_item_id;
    bool toggle_inventory_mode = false;
    int seq = 0;
    float local_dt = 0.0f;
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
    bool grappling_pressed = false;
    int request_rune_type = -1;
    std::string request_item_id;
    bool toggle_inventory_mode = false;
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
    float mana = 0.0f;
    float max_mana = 0.0f;
    float grappling_cooldown_remaining = 0.0f;
    float grappling_cooldown_total = 0.0f;
    std::vector<float> rune_cooldown_remaining;
    std::vector<float> rune_cooldown_total;
    struct StatusEffectSnapshot {
        int type = 0;
        float remaining_seconds = 0.0f;
        float total_seconds = 0.0f;
        float magnitude_per_second = 0.0f;
        std::string composite_effect_id;

        bool operator==(const StatusEffectSnapshot& other) const {
            return type == other.type && remaining_seconds == other.remaining_seconds &&
                   total_seconds == other.total_seconds && magnitude_per_second == other.magnitude_per_second &&
                   composite_effect_id == other.composite_effect_id;
        }
    };
    std::vector<StatusEffectSnapshot> status_effects;
    std::vector<std::string> item_slots;
    std::vector<int> item_slot_counts;
    std::vector<float> item_slot_cooldown_remaining;
    std::vector<float> item_slot_cooldown_total;
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
    bool active = false;
    bool volatile_cast = false;
    float activation_total_seconds = 0.0f;
    float activation_remaining_seconds = 0.0f;
    bool creates_influence_zone = true;
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

struct FireStormDummySnapshot {
    int id = -1;
    int owner_player_id = -1;
    int owner_team = 0;
    int cell_x = 0;
    int cell_y = 0;
    int state = 0;
    float state_time = 0.0f;
    float state_duration = 0.0f;
    float idle_lifetime_remaining_seconds = -1.0f;
    bool alive = true;
};

struct FireStormCastSnapshot {
    int id = -1;
    int owner_player_id = -1;
    int owner_team = 0;
    int center_cell_x = 0;
    int center_cell_y = 0;
    float elapsed_seconds = 0.0f;
    float duration_seconds = 0.0f;
    bool alive = true;
};

struct GrapplingHookSnapshot {
    int id = -1;
    int owner_player_id = -1;
    int owner_team = 0;
    float head_pos_x = 0.0f;
    float head_pos_y = 0.0f;
    float target_pos_x = 0.0f;
    float target_pos_y = 0.0f;
    float latch_point_x = 0.0f;
    float latch_point_y = 0.0f;
    float pull_destination_x = 0.0f;
    float pull_destination_y = 0.0f;
    int phase = 0;
    int latch_target_type = 0;
    int latch_target_id = -1;
    int latch_cell_x = 0;
    int latch_cell_y = 0;
    bool latched = false;
    float animation_time = 0.0f;
    float pull_elapsed_seconds = 0.0f;
    float max_pull_duration_seconds = 0.0f;
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
    std::vector<FireStormDummySnapshot> fire_storm_dummies;
    std::vector<int> removed_fire_storm_dummy_ids;
    std::vector<FireStormCastSnapshot> fire_storm_casts;
    std::vector<int> removed_fire_storm_cast_ids;
    std::vector<GrapplingHookSnapshot> grappling_hooks;
    std::vector<int> removed_grappling_hook_ids;
};

struct LobbyPlayerInfo {
    int player_id = -1;
    std::string name;
};

struct LobbyStateMessage {
    std::vector<LobbyPlayerInfo> players;
    bool host_can_start = false;
    int mode_type = 0;
    int round_time_seconds = 0;
    int best_of_target_kills = 0;
    float shrink_tiles_per_second = 0.0f;
    float shrink_start_seconds = 0.0f;
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
