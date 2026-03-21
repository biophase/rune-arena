#include "net/network_manager.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <unordered_map>

#include <enet/enet.h>

#include "core/constants.h"
#include "net/binary_protocol.h"

namespace {

constexpr enet_uint8 kChannelReliable = 0;
constexpr enet_uint8 kChannelRealtime = 1;

void NetLog(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    std::printf("\n");
    va_end(args);
}

std::string AddressToString(const ENetAddress& address) {
    char ip[64] = {};
    enet_address_get_host_ip(&address, ip, sizeof(ip));
    return std::string(ip);
}

bool AreEqual(const PlayerSnapshot& a, const PlayerSnapshot& b) {
    return a.id == b.id && a.team == b.team && a.pos_x == b.pos_x && a.pos_y == b.pos_y && a.vel_x == b.vel_x &&
           a.vel_y == b.vel_y && a.aim_dir_x == b.aim_dir_x && a.aim_dir_y == b.aim_dir_y && a.hp == b.hp &&
           a.kills == b.kills && a.alive == b.alive && a.facing == b.facing && a.action_state == b.action_state &&
           a.melee_active_remaining == b.melee_active_remaining && a.rune_placing_mode == b.rune_placing_mode &&
           a.selected_rune_type == b.selected_rune_type && a.rune_place_cooldown_remaining == b.rune_place_cooldown_remaining &&
           a.mana == b.mana && a.max_mana == b.max_mana &&
           a.grappling_cooldown_remaining == b.grappling_cooldown_remaining &&
           a.grappling_cooldown_total == b.grappling_cooldown_total &&
           a.rune_cooldown_remaining == b.rune_cooldown_remaining &&
           a.rune_cooldown_total == b.rune_cooldown_total && a.status_effects == b.status_effects &&
           a.item_slots == b.item_slots &&
           a.item_slot_counts == b.item_slot_counts &&
           a.item_slot_cooldown_remaining == b.item_slot_cooldown_remaining &&
           a.item_slot_cooldown_total == b.item_slot_cooldown_total &&
           a.awaiting_respawn == b.awaiting_respawn && a.respawn_remaining == b.respawn_remaining &&
           a.last_processed_move_seq == b.last_processed_move_seq;
}

bool AreEqual(const GrapplingHookSnapshot& a, const GrapplingHookSnapshot& b) {
    return a.id == b.id && a.owner_player_id == b.owner_player_id && a.owner_team == b.owner_team &&
           a.head_pos_x == b.head_pos_x && a.head_pos_y == b.head_pos_y &&
           a.target_pos_x == b.target_pos_x && a.target_pos_y == b.target_pos_y &&
           a.latch_point_x == b.latch_point_x && a.latch_point_y == b.latch_point_y &&
           a.pull_destination_x == b.pull_destination_x && a.pull_destination_y == b.pull_destination_y &&
           a.phase == b.phase && a.latch_target_type == b.latch_target_type &&
           a.latch_target_id == b.latch_target_id && a.latch_cell_x == b.latch_cell_x &&
           a.latch_cell_y == b.latch_cell_y && a.latched == b.latched &&
           a.animation_time == b.animation_time &&
           a.pull_elapsed_seconds == b.pull_elapsed_seconds &&
           a.max_pull_duration_seconds == b.max_pull_duration_seconds &&
           a.alive == b.alive;
}

bool AreEqual(const RuneSnapshot& a, const RuneSnapshot& b) {
    return a.id == b.id && a.owner_player_id == b.owner_player_id && a.owner_team == b.owner_team && a.x == b.x &&
           a.y == b.y && a.rune_type == b.rune_type && a.placement_order == b.placement_order &&
           a.active == b.active && a.volatile_cast == b.volatile_cast &&
           a.activation_total_seconds == b.activation_total_seconds &&
           a.activation_remaining_seconds == b.activation_remaining_seconds &&
           a.creates_influence_zone == b.creates_influence_zone;
}

bool AreEqual(const ProjectileSnapshot& a, const ProjectileSnapshot& b) {
    return a.id == b.id && a.owner_player_id == b.owner_player_id && a.owner_team == b.owner_team &&
           a.pos_x == b.pos_x && a.pos_y == b.pos_y && a.vel_x == b.vel_x && a.vel_y == b.vel_y &&
           a.radius == b.radius && a.damage == b.damage && a.animation_key == b.animation_key &&
           a.emitter_enabled == b.emitter_enabled && a.emitter_emit_every_frames == b.emitter_emit_every_frames &&
           a.emitter_frame_counter == b.emitter_frame_counter && a.alive == b.alive;
}

bool AreEqual(const IceWallSnapshot& a, const IceWallSnapshot& b) {
    return a.id == b.id && a.owner_player_id == b.owner_player_id && a.owner_team == b.owner_team &&
           a.cell_x == b.cell_x && a.cell_y == b.cell_y && a.state == b.state && a.state_time == b.state_time &&
           a.hp == b.hp && a.alive == b.alive;
}

bool AreEqual(const MapObjectSnapshot& a, const MapObjectSnapshot& b) {
    return a.id == b.id && a.prototype_id == b.prototype_id && a.cell_x == b.cell_x && a.cell_y == b.cell_y &&
           a.object_type == b.object_type && a.hp == b.hp && a.state == b.state &&
           a.state_time == b.state_time && a.death_duration == b.death_duration &&
           a.collision_enabled == b.collision_enabled && a.alive == b.alive;
}

bool AreEqual(const FireStormDummySnapshot& a, const FireStormDummySnapshot& b) {
    return a.id == b.id && a.owner_player_id == b.owner_player_id && a.owner_team == b.owner_team &&
           a.cell_x == b.cell_x && a.cell_y == b.cell_y && a.state == b.state &&
           a.state_time == b.state_time && a.state_duration == b.state_duration &&
           a.idle_lifetime_remaining_seconds == b.idle_lifetime_remaining_seconds && a.alive == b.alive;
}

bool AreEqual(const FireStormCastSnapshot& a, const FireStormCastSnapshot& b) {
    return a.id == b.id && a.owner_player_id == b.owner_player_id && a.owner_team == b.owner_team &&
           a.center_cell_x == b.center_cell_x && a.center_cell_y == b.center_cell_y &&
           a.elapsed_seconds == b.elapsed_seconds && a.duration_seconds == b.duration_seconds && a.alive == b.alive;
}

template <typename T>
std::unordered_map<int, const T*> BuildIdMap(const std::vector<T>& entities) {
    std::unordered_map<int, const T*> by_id;
    by_id.reserve(entities.size());
    for (const auto& entity : entities) {
        by_id[entity.id] = &entity;
    }
    return by_id;
}

template <typename T>
void UpsertById(std::vector<T>& entities, const T& incoming) {
    for (auto& entity : entities) {
        if (entity.id == incoming.id) {
            entity = incoming;
            return;
        }
    }
    entities.push_back(incoming);
}

template <typename T>
void RemoveByIds(std::vector<T>& entities, const std::vector<int>& removed_ids) {
    if (removed_ids.empty()) {
        return;
    }
    entities.erase(std::remove_if(entities.begin(), entities.end(),
                                  [&](const T& entity) {
                                      return std::find(removed_ids.begin(), removed_ids.end(), entity.id) !=
                                             removed_ids.end();
                                  }),
                   entities.end());
}

ServerSnapshotMessage BuildDeltaSnapshot(const ServerSnapshotMessage& base, const ServerSnapshotMessage& current) {
    ServerSnapshotMessage delta = current;
    delta.is_delta = true;
    delta.base_snapshot_id = base.snapshot_id;

    delta.players.clear();
    delta.removed_player_ids.clear();
    delta.runes.clear();
    delta.removed_rune_ids.clear();
    delta.projectiles.clear();
    delta.removed_projectile_ids.clear();
    delta.ice_walls.clear();
    delta.removed_ice_wall_ids.clear();
    delta.map_objects.clear();
    delta.removed_map_object_ids.clear();
    delta.fire_storm_dummies.clear();
    delta.removed_fire_storm_dummy_ids.clear();
    delta.fire_storm_casts.clear();
    delta.removed_fire_storm_cast_ids.clear();
    delta.grappling_hooks.clear();
    delta.removed_grappling_hook_ids.clear();

    const auto base_players = BuildIdMap(base.players);
    const auto current_players = BuildIdMap(current.players);
    for (const auto& player : current.players) {
        auto it = base_players.find(player.id);
        if (it == base_players.end() || !AreEqual(player, *it->second)) {
            delta.players.push_back(player);
        }
    }
    for (const auto& player : base.players) {
        if (current_players.find(player.id) == current_players.end()) {
            delta.removed_player_ids.push_back(player.id);
        }
    }

    const auto base_runes = BuildIdMap(base.runes);
    const auto current_runes = BuildIdMap(current.runes);
    for (const auto& rune : current.runes) {
        auto it = base_runes.find(rune.id);
        if (it == base_runes.end() || !AreEqual(rune, *it->second)) {
            delta.runes.push_back(rune);
        }
    }
    for (const auto& rune : base.runes) {
        if (current_runes.find(rune.id) == current_runes.end()) {
            delta.removed_rune_ids.push_back(rune.id);
        }
    }

    const auto base_projectiles = BuildIdMap(base.projectiles);
    const auto current_projectiles = BuildIdMap(current.projectiles);
    for (const auto& projectile : current.projectiles) {
        auto it = base_projectiles.find(projectile.id);
        if (it == base_projectiles.end() || !AreEqual(projectile, *it->second)) {
            delta.projectiles.push_back(projectile);
        }
    }
    for (const auto& projectile : base.projectiles) {
        if (current_projectiles.find(projectile.id) == current_projectiles.end()) {
            delta.removed_projectile_ids.push_back(projectile.id);
        }
    }

    const auto base_walls = BuildIdMap(base.ice_walls);
    const auto current_walls = BuildIdMap(current.ice_walls);
    for (const auto& wall : current.ice_walls) {
        auto it = base_walls.find(wall.id);
        if (it == base_walls.end() || !AreEqual(wall, *it->second)) {
            delta.ice_walls.push_back(wall);
        }
    }
    for (const auto& wall : base.ice_walls) {
        if (current_walls.find(wall.id) == current_walls.end()) {
            delta.removed_ice_wall_ids.push_back(wall.id);
        }
    }

    const auto base_objects = BuildIdMap(base.map_objects);
    const auto current_objects = BuildIdMap(current.map_objects);
    for (const auto& object : current.map_objects) {
        auto it = base_objects.find(object.id);
        if (it == base_objects.end() || !AreEqual(object, *it->second)) {
            delta.map_objects.push_back(object);
        }
    }
    for (const auto& object : base.map_objects) {
        if (current_objects.find(object.id) == current_objects.end()) {
            delta.removed_map_object_ids.push_back(object.id);
        }
    }

    const auto base_dummies = BuildIdMap(base.fire_storm_dummies);
    const auto current_dummies = BuildIdMap(current.fire_storm_dummies);
    for (const auto& dummy : current.fire_storm_dummies) {
        auto it = base_dummies.find(dummy.id);
        if (it == base_dummies.end() || !AreEqual(dummy, *it->second)) {
            delta.fire_storm_dummies.push_back(dummy);
        }
    }
    for (const auto& dummy : base.fire_storm_dummies) {
        if (current_dummies.find(dummy.id) == current_dummies.end()) {
            delta.removed_fire_storm_dummy_ids.push_back(dummy.id);
        }
    }

    const auto base_casts = BuildIdMap(base.fire_storm_casts);
    const auto current_casts = BuildIdMap(current.fire_storm_casts);
    for (const auto& cast : current.fire_storm_casts) {
        auto it = base_casts.find(cast.id);
        if (it == base_casts.end() || !AreEqual(cast, *it->second)) {
            delta.fire_storm_casts.push_back(cast);
        }
    }
    for (const auto& cast : base.fire_storm_casts) {
        if (current_casts.find(cast.id) == current_casts.end()) {
            delta.removed_fire_storm_cast_ids.push_back(cast.id);
        }
    }

    const auto base_hooks = BuildIdMap(base.grappling_hooks);
    const auto current_hooks = BuildIdMap(current.grappling_hooks);
    for (const auto& hook : current.grappling_hooks) {
        auto it = base_hooks.find(hook.id);
        if (it == base_hooks.end() || !AreEqual(hook, *it->second)) {
            delta.grappling_hooks.push_back(hook);
        }
    }
    for (const auto& hook : base.grappling_hooks) {
        if (current_hooks.find(hook.id) == current_hooks.end()) {
            delta.removed_grappling_hook_ids.push_back(hook.id);
        }
    }

    return delta;
}

std::optional<ServerSnapshotMessage> ApplyDeltaSnapshot(const ServerSnapshotMessage& base,
                                                        const ServerSnapshotMessage& delta) {
    if (!delta.is_delta || delta.base_snapshot_id != base.snapshot_id) {
        return std::nullopt;
    }

    ServerSnapshotMessage out = base;
    out.server_tick = delta.server_tick;
    out.snapshot_id = delta.snapshot_id;
    out.base_snapshot_id = 0;
    out.is_delta = false;
    out.time_remaining = delta.time_remaining;
    out.shrink_tiles_per_second = delta.shrink_tiles_per_second;
    out.min_arena_radius_tiles = delta.min_arena_radius_tiles;
    out.arena_radius_tiles = delta.arena_radius_tiles;
    out.arena_radius_world = delta.arena_radius_world;
    out.arena_center_world_x = delta.arena_center_world_x;
    out.arena_center_world_y = delta.arena_center_world_y;
    out.match_running = delta.match_running;
    out.match_finished = delta.match_finished;
    out.red_team_kills = delta.red_team_kills;
    out.blue_team_kills = delta.blue_team_kills;

    RemoveByIds(out.players, delta.removed_player_ids);
    for (const auto& player : delta.players) {
        UpsertById(out.players, player);
    }

    RemoveByIds(out.runes, delta.removed_rune_ids);
    for (const auto& rune : delta.runes) {
        UpsertById(out.runes, rune);
    }

    RemoveByIds(out.projectiles, delta.removed_projectile_ids);
    for (const auto& projectile : delta.projectiles) {
        UpsertById(out.projectiles, projectile);
    }

    RemoveByIds(out.ice_walls, delta.removed_ice_wall_ids);
    for (const auto& wall : delta.ice_walls) {
        UpsertById(out.ice_walls, wall);
    }

    RemoveByIds(out.map_objects, delta.removed_map_object_ids);
    for (const auto& object : delta.map_objects) {
        UpsertById(out.map_objects, object);
    }
    RemoveByIds(out.fire_storm_dummies, delta.removed_fire_storm_dummy_ids);
    for (const auto& dummy : delta.fire_storm_dummies) {
        UpsertById(out.fire_storm_dummies, dummy);
    }
    RemoveByIds(out.fire_storm_casts, delta.removed_fire_storm_cast_ids);
    for (const auto& cast : delta.fire_storm_casts) {
        UpsertById(out.fire_storm_casts, cast);
    }
    RemoveByIds(out.grappling_hooks, delta.removed_grappling_hook_ids);
    for (const auto& hook : delta.grappling_hooks) {
        UpsertById(out.grappling_hooks, hook);
    }

    out.removed_player_ids.clear();
    out.removed_rune_ids.clear();
    out.removed_projectile_ids.clear();
    out.removed_ice_wall_ids.clear();
    out.removed_map_object_ids.clear();
    out.removed_fire_storm_dummy_ids.clear();
    out.removed_fire_storm_cast_ids.clear();
    out.removed_grappling_hook_ids.clear();
    return out;
}

void TrimSnapshotHistory(std::unordered_map<int, ServerSnapshotMessage>& history, int newest_snapshot_id) {
    const int min_snapshot_id = newest_snapshot_id - Constants::kNetworkSnapshotHistorySize;
    for (auto it = history.begin(); it != history.end();) {
        if (it->first < min_snapshot_id) {
            it = history.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace

NetworkManager::NetworkManager() = default;

NetworkManager::~NetworkManager() { Stop(); }

bool NetworkManager::EnsureEnetInitialized() {
    if (enet_initialized_) {
        return true;
    }

    if (enet_initialize() != 0) {
        return false;
    }

    enet_initialized_ = true;
    return true;
}

bool NetworkManager::StartHost(int port) {
    Stop();
    if (!EnsureEnetInitialized()) {
        last_debug_message_ = "enet initialization failed";
        NetLog("[NET] Host start failed: %s", last_debug_message_.c_str());
        return false;
    }

    ENetAddress address;
    address.host = ENET_HOST_ANY;
    address.port = static_cast<enet_uint16>(port);

    host_ = enet_host_create(&address, 16, 2, 0, 0);
    if (!host_) {
        last_debug_message_ = "failed to bind/listen on UDP port (in use or blocked)";
        NetLog("[NET] Host start failed on UDP %d: %s", port, last_debug_message_.c_str());
        return false;
    }

    is_host_ = true;
    connected_ = true;
    assigned_local_player_id_ = 0;
    client_connection_state_ = ClientConnectionState::Idle;
    last_debug_message_ = "host listening";
    NetLog("[NET] Host started on UDP port %d", port);
    return true;
}

bool NetworkManager::StartClient() {
    Stop();
    if (!EnsureEnetInitialized()) {
        last_debug_message_ = "enet initialization failed";
        NetLog("[NET] Client start failed: %s", last_debug_message_.c_str());
        return false;
    }

    host_ = enet_host_create(nullptr, 1, 2, 0, 0);
    if (!host_) {
        last_debug_message_ = "failed to create ENet client host";
        NetLog("[NET] Client start failed: %s", last_debug_message_.c_str());
        return false;
    }

    is_host_ = false;
    connected_ = false;
    client_connection_state_ = ClientConnectionState::Disconnected;
    last_debug_message_ = "client initialized";
    return true;
}

bool NetworkManager::ConnectToHost(const std::string& ip, int port) {
    if (!host_ || is_host_) {
        return false;
    }

    ENetAddress address;
    address.port = static_cast<enet_uint16>(port);
    if (enet_address_set_host(&address, ip.c_str()) != 0) {
        last_debug_message_ = "failed to resolve host address";
        client_connection_state_ = ClientConnectionState::Disconnected;
        return false;
    }

    assigned_local_player_id_ = -1;
    latest_lobby_state_.reset();
    latest_snapshot_.reset();
    client_snapshot_history_.clear();
    last_client_applied_snapshot_id_ = 0;
    client_received_lobby_state_ = false;
    pending_match_start_ = false;
    client_connection_state_ = ClientConnectionState::ConnectingTransport;
    last_debug_message_ = "connecting transport";

    server_peer_ = enet_host_connect(host_, &address, 2, 0);
    if (server_peer_) {
        NetLog("[NET] Client connecting to %s:%d", ip.c_str(), port);
    } else {
        NetLog("[NET] Client failed to start connect to %s:%d", ip.c_str(), port);
        client_connection_state_ = ClientConnectionState::Disconnected;
        last_debug_message_ = "connect start failed";
    }
    return server_peer_ != nullptr;
}

void NetworkManager::Stop() {
    if (host_) {
        enet_host_destroy(host_);
        host_ = nullptr;
    }

    server_peer_ = nullptr;
    peers_.clear();
    host_snapshot_history_.clear();
    client_snapshot_history_.clear();
    last_client_applied_snapshot_id_ = 0;
    pending_host_moves_.clear();
    pending_host_actions_.clear();
    latest_snapshot_.reset();
    latest_lobby_state_.reset();
    pending_match_start_ = false;
    client_received_lobby_state_ = false;

    is_host_ = false;
    connected_ = false;
    assigned_local_player_id_ = -1;
    next_remote_player_id_ = 1;
    client_connection_state_ = ClientConnectionState::Idle;
    last_debug_message_ = "stopped";
    last_received_snapshot_tick_ = -1;
    rate_window_bytes_sent_ = 0;
    rate_window_bytes_received_ = 0;
    rate_window_packets_sent_ = 0;
    rate_window_packets_received_ = 0;
    rate_window_reconciliation_corrections_ = 0;
    last_rate_update_ms_ = enet_time_get();
    telemetry_ = NetTelemetry{};

    if (enet_initialized_) {
        enet_deinitialize();
        enet_initialized_ = false;
    }
}

void NetworkManager::SetLocalPlayerName(const std::string& player_name) { local_player_name_ = player_name; }

void NetworkManager::Poll() {
    if (!host_) {
        return;
    }

    ENetEvent event;
    while (enet_host_service(host_, &event, 0) > 0) {
        switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT: {
                if (is_host_) {
                    PeerInfo info;
                    info.player_id = next_remote_player_id_++;
                    info.name = "Player" + std::to_string(info.player_id);
                    peers_[event.peer] = info;
                    NetLog("[NET] Host transport connect from %s:%u -> assigned temp id=%d",
                           AddressToString(event.peer->address).c_str(), event.peer->address.port, info.player_id);
                } else {
                    connected_ = true;
                    client_connection_state_ = ClientConnectionState::WaitingJoinAck;
                    last_debug_message_ = "transport connected, waiting join_ack";
                    SendPacketToPeer(server_peer_, binary::EncodeJoinPacket(local_player_name_), true, kChannelReliable,
                                     false);
                    NetLog("[NET] Client transport connected, sent join name='%s'", local_player_name_.c_str());
                }
                break;
            }
            case ENET_EVENT_TYPE_RECEIVE: {
                binary::DecodedPacketHeader header;
                if (!binary::DecodePacketHeader(event.packet->data, event.packet->dataLength, header)) {
                    NetLog("[NET] Dropped malformed packet (%zu bytes)", static_cast<size_t>(event.packet->dataLength));
                    enet_packet_destroy(event.packet);
                    break;
                }
                if (!header.version_ok) {
                    last_debug_message_ = "protocol version mismatch";
                    NetLog("[NET] Dropped packet with protocol version mismatch");
                    enet_packet_destroy(event.packet);
                    break;
                }

                if (is_host_) {
                    switch (header.type) {
                        case binary::PacketType::Join: {
                            const auto name = binary::DecodeJoinPayload(header.payload, header.payload_size);
                            const auto it = peers_.find(event.peer);
                            if (name.has_value() && it != peers_.end()) {
                                it->second.name = *name;
                                SendPacketToPeer(event.peer, binary::EncodeJoinAckPacket(it->second.player_id), true,
                                                 kChannelReliable, false);
                                NetLog("[NET] Host received join from '%s' (%s), sent join_ack player_id=%d",
                                       it->second.name.c_str(), AddressToString(event.peer->address).c_str(),
                                       it->second.player_id);
                            }
                            break;
                        }
                        case binary::PacketType::ClientMove: {
                            auto move = binary::DecodeClientMovePayload(header.payload, header.payload_size);
                            if (move.has_value()) {
                                auto it = peers_.find(event.peer);
                                if (it != peers_.end()) {
                                    it->second.last_acked_snapshot_id =
                                        std::max(it->second.last_acked_snapshot_id, move->last_received_snapshot_id);
                                }
                                pending_host_moves_.push_back(*move);
                            }
                            break;
                        }
                        case binary::PacketType::ClientAction: {
                            auto action = binary::DecodeClientActionPayload(header.payload, header.payload_size);
                            if (action.has_value()) {
                                auto it = peers_.find(event.peer);
                                if (it != peers_.end()) {
                                    it->second.last_acked_snapshot_id =
                                        std::max(it->second.last_acked_snapshot_id, action->last_received_snapshot_id);
                                }
                                pending_host_actions_.push_back(*action);
                            }
                            break;
                        }
                        default:
                            break;
                    }
                } else {
                    switch (header.type) {
                        case binary::PacketType::JoinAck: {
                            const auto player_id = binary::DecodeJoinAckPayload(header.payload, header.payload_size);
                            if (!player_id.has_value()) {
                                break;
                            }
                            assigned_local_player_id_ = *player_id;
                            client_connection_state_ =
                                client_received_lobby_state_ ? ClientConnectionState::ReadyInLobby
                                                             : ClientConnectionState::WaitingLobbyState;
                            last_debug_message_ =
                                client_received_lobby_state_ ? "join_ack + lobby_state received" : "join_ack received";
                            NetLog("[NET] Client received join_ack player_id=%d", assigned_local_player_id_);
                            break;
                        }
                        case binary::PacketType::Snapshot: {
                            auto received_snapshot = binary::DecodeSnapshotPayload(header.payload, header.payload_size);
                            if (!received_snapshot.has_value()) {
                                break;
                            }
                            if (received_snapshot->is_delta) {
                                telemetry_.delta_snapshots_received_total += 1;
                            } else {
                                telemetry_.keyframe_snapshots_received_total += 1;
                            }

                            std::optional<ServerSnapshotMessage> applied_snapshot;
                            if (received_snapshot->is_delta) {
                                auto base_it = client_snapshot_history_.find(received_snapshot->base_snapshot_id);
                                if (base_it != client_snapshot_history_.end()) {
                                    applied_snapshot = ApplyDeltaSnapshot(base_it->second, *received_snapshot);
                                } else {
                                    telemetry_.dropped_snapshots_total += 1;
                                    telemetry_.dropped_delta_missing_base_total += 1;
                                    last_debug_message_ = "dropped delta: missing base snapshot";
                                }
                            } else {
                                applied_snapshot = *received_snapshot;
                            }

                            if (applied_snapshot.has_value()) {
                                latest_snapshot_ = *applied_snapshot;
                                last_client_applied_snapshot_id_ = latest_snapshot_->snapshot_id;
                                client_snapshot_history_[latest_snapshot_->snapshot_id] = *latest_snapshot_;
                                TrimSnapshotHistory(client_snapshot_history_, latest_snapshot_->snapshot_id);

                                const int tick = latest_snapshot_->server_tick;
                                if (last_received_snapshot_tick_ >= 0 && tick > last_received_snapshot_tick_ + 1) {
                                    telemetry_.dropped_snapshots_total +=
                                        static_cast<uint64_t>(tick - (last_received_snapshot_tick_ + 1));
                                }
                                if (tick > last_received_snapshot_tick_) {
                                    last_received_snapshot_tick_ = tick;
                                }
                            }
                            break;
                        }
                        case binary::PacketType::LobbyState: {
                            latest_lobby_state_ = binary::DecodeLobbyStatePayload(header.payload, header.payload_size);
                            client_received_lobby_state_ = latest_lobby_state_.has_value();
                            if (assigned_local_player_id_ >= 0) {
                                client_connection_state_ = ClientConnectionState::ReadyInLobby;
                                last_debug_message_ = "lobby_state received";
                            } else if (connected_) {
                                client_connection_state_ = ClientConnectionState::WaitingJoinAck;
                                last_debug_message_ = "lobby_state received before join_ack";
                            }
                            if (latest_lobby_state_.has_value()) {
                                NetLog("[NET] Client received lobby_state with %zu players",
                                       latest_lobby_state_->players.size());
                            }
                            break;
                        }
                        case binary::PacketType::MatchStart: {
                            const auto match_start =
                                binary::DecodeMatchStartPayload(header.payload, header.payload_size);
                            if (match_start.has_value() && match_start->start) {
                                pending_match_start_ = true;
                                NetLog("[NET] Client received match_start");
                            }
                            break;
                        }
                        default:
                            break;
                    }
                }

                RegisterIncomingPacket(event.packet->dataLength, header.type == binary::PacketType::Snapshot);

                enet_packet_destroy(event.packet);
                break;
            }
            case ENET_EVENT_TYPE_DISCONNECT: {
                if (is_host_) {
                    NetLog("[NET] Host disconnect from %s:%u", AddressToString(event.peer->address).c_str(),
                           event.peer->address.port);
                    peers_.erase(event.peer);
                } else {
                    connected_ = false;
                    server_peer_ = nullptr;
                    client_connection_state_ = ClientConnectionState::Disconnected;
                    last_debug_message_ = "disconnected";
                    NetLog("[NET] Client disconnected from host");
                }
                break;
            }
            case ENET_EVENT_TYPE_NONE:
                break;
        }
    }
    UpdateRateTelemetry();
}

bool NetworkManager::IsHost() const { return is_host_; }

bool NetworkManager::IsConnected() const {
    if (is_host_) {
        return host_ != nullptr;
    }
    return connected_;
}

ClientConnectionState NetworkManager::GetClientConnectionState() const { return client_connection_state_; }

bool NetworkManager::HasReceivedJoinAck() const { return assigned_local_player_id_ >= 0; }

bool NetworkManager::HasReceivedLobbyState() const { return client_received_lobby_state_; }

const std::string& NetworkManager::GetLastDebugMessage() const { return last_debug_message_; }

void NetworkManager::SendClientMove(const ClientMoveMessage& message) {
    if (is_host_ || !server_peer_) {
        return;
    }
    ClientMoveMessage outbound = message;
    outbound.last_received_snapshot_id = last_client_applied_snapshot_id_;
    SendPacketToPeer(server_peer_, binary::EncodeClientMovePacket(outbound), false, kChannelRealtime, false);
}

void NetworkManager::SendClientAction(const ClientActionMessage& message) {
    if (is_host_ || !server_peer_) {
        return;
    }
    ClientActionMessage outbound = message;
    outbound.last_received_snapshot_id = last_client_applied_snapshot_id_;
    SendPacketToPeer(server_peer_, binary::EncodeClientActionPacket(outbound), true, kChannelReliable, false);
}

std::vector<ClientMoveMessage> NetworkManager::ConsumeHostMoveInputs() {
    std::vector<ClientMoveMessage> out;
    out.swap(pending_host_moves_);
    return out;
}

std::vector<ClientActionMessage> NetworkManager::ConsumeHostActionInputs() {
    std::vector<ClientActionMessage> out;
    out.swap(pending_host_actions_);
    return out;
}

void NetworkManager::BroadcastSnapshot(const ServerSnapshotMessage& message) {
    if (!is_host_) {
        return;
    }

    ServerSnapshotMessage full = message;
    if (full.snapshot_id <= 0) {
        full.snapshot_id = full.server_tick;
    }
    if (full.snapshot_id <= 0) {
        return;
    }
    full.base_snapshot_id = 0;
    full.is_delta = false;
    full.removed_player_ids.clear();
    full.removed_rune_ids.clear();
    full.removed_projectile_ids.clear();
    full.removed_ice_wall_ids.clear();
    full.removed_map_object_ids.clear();

    host_snapshot_history_[full.snapshot_id] = full;
    TrimSnapshotHistory(host_snapshot_history_, full.snapshot_id);

    const std::vector<uint8_t> full_packet = binary::EncodeSnapshotPacket(full);

    for (auto& [peer, peer_info] : peers_) {
        const bool force_keyframe = (peer_info.last_keyframe_snapshot_id_sent <= 0) ||
                                    ((full.snapshot_id - peer_info.last_keyframe_snapshot_id_sent) >=
                                     Constants::kNetworkSnapshotKeyframeIntervalTicks);
        bool sent = false;
        if (!force_keyframe && peer_info.last_acked_snapshot_id > 0) {
            auto base_it = host_snapshot_history_.find(peer_info.last_acked_snapshot_id);
            if (base_it != host_snapshot_history_.end() && base_it->first < full.snapshot_id) {
                ServerSnapshotMessage delta = BuildDeltaSnapshot(base_it->second, full);
                const std::vector<uint8_t> delta_packet = binary::EncodeSnapshotPacket(delta);
                if (delta_packet.size() < full_packet.size()) {
                    SendPacketToPeer(peer, delta_packet, false, kChannelRealtime, true);
                    telemetry_.delta_snapshots_sent_total += 1;
                    sent = true;
                }
            }
        }
        if (!sent) {
            SendPacketToPeer(peer, full_packet, false, kChannelRealtime, true);
            telemetry_.keyframe_snapshots_sent_total += 1;
            peer_info.last_keyframe_snapshot_id_sent = full.snapshot_id;
        }
    }
}

std::optional<ServerSnapshotMessage> NetworkManager::ConsumeLatestSnapshot() {
    auto out = latest_snapshot_;
    latest_snapshot_.reset();
    return out;
}

void NetworkManager::BroadcastLobbyState(const LobbyStateMessage& message) {
    if (!is_host_) {
        return;
    }
    BroadcastPacket(binary::EncodeLobbyStatePacket(message), true, kChannelReliable, false);
}

std::optional<LobbyStateMessage> NetworkManager::ConsumeLobbyState() {
    auto out = latest_lobby_state_;
    latest_lobby_state_.reset();
    return out;
}

void NetworkManager::BroadcastMatchStart(const MatchStartMessage& message) {
    if (!is_host_) {
        return;
    }
    BroadcastPacket(binary::EncodeMatchStartPacket(message), true, kChannelReliable, false);
}

bool NetworkManager::ConsumeMatchStart() {
    const bool out = pending_match_start_;
    pending_match_start_ = false;
    return out;
}

int NetworkManager::GetAssignedLocalPlayerId() const { return assigned_local_player_id_; }

std::vector<RemotePlayerInfo> NetworkManager::GetRemotePlayers() const {
    std::vector<RemotePlayerInfo> result;
    for (const auto& [_, peer] : peers_) {
        result.push_back({peer.player_id, peer.name});
    }
    return result;
}

const NetTelemetry& NetworkManager::GetTelemetry() const { return telemetry_; }

void NetworkManager::AddReconciliationCorrection() {
    telemetry_.reconciliation_corrections_total += 1;
    rate_window_reconciliation_corrections_ += 1;
}

void NetworkManager::SendPacketToPeer(ENetPeer* peer, const std::vector<uint8_t>& packet_data, bool reliable,
                                      uint8_t channel, bool is_snapshot) {
    if (!peer) {
        return;
    }

    ENetPacket* packet =
        enet_packet_create(packet_data.data(), packet_data.size(), reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    if (packet) {
        enet_peer_send(peer, channel, packet);
        RegisterOutgoingPacket(packet_data.size(), is_snapshot);
        enet_host_flush(host_);
    }
}

void NetworkManager::BroadcastPacket(const std::vector<uint8_t>& packet_data, bool reliable, uint8_t channel,
                                     bool is_snapshot) {
    if (!host_) {
        return;
    }

    ENetPacket* packet =
        enet_packet_create(packet_data.data(), packet_data.size(), reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    if (packet) {
        enet_host_broadcast(host_, channel, packet);
        RegisterOutgoingPacket(packet_data.size(), is_snapshot);
        enet_host_flush(host_);
    }
}

void NetworkManager::RegisterOutgoingPacket(size_t bytes, bool is_snapshot) {
    telemetry_.bytes_sent_total += static_cast<uint64_t>(bytes);
    telemetry_.packets_sent_total += 1;
    rate_window_bytes_sent_ += static_cast<uint64_t>(bytes);
    rate_window_packets_sent_ += 1;
    if (is_snapshot) {
        telemetry_.snapshot_bytes_sent_total += static_cast<uint64_t>(bytes);
        telemetry_.snapshots_sent_total += 1;
        telemetry_.average_snapshot_bytes_sent =
            static_cast<float>(telemetry_.snapshot_bytes_sent_total) /
            static_cast<float>(std::max<uint64_t>(1, telemetry_.snapshots_sent_total));
    }
}

void NetworkManager::RegisterIncomingPacket(size_t bytes, bool is_snapshot) {
    telemetry_.bytes_received_total += static_cast<uint64_t>(bytes);
    telemetry_.packets_received_total += 1;
    rate_window_bytes_received_ += static_cast<uint64_t>(bytes);
    rate_window_packets_received_ += 1;
    if (is_snapshot) {
        telemetry_.snapshot_bytes_received_total += static_cast<uint64_t>(bytes);
        telemetry_.snapshots_received_total += 1;
        telemetry_.average_snapshot_bytes_received =
            static_cast<float>(telemetry_.snapshot_bytes_received_total) /
            static_cast<float>(std::max<uint64_t>(1, telemetry_.snapshots_received_total));
    }
}

void NetworkManager::UpdateRateTelemetry() {
    const uint32_t now = enet_time_get();
    if (last_rate_update_ms_ == 0) {
        last_rate_update_ms_ = now;
        return;
    }
    const uint32_t elapsed_ms = now - last_rate_update_ms_;
    if (elapsed_ms < 1000) {
        return;
    }

    const float elapsed_seconds = static_cast<float>(elapsed_ms) / 1000.0f;
    telemetry_.bytes_per_sec_up = static_cast<float>(rate_window_bytes_sent_) / elapsed_seconds;
    telemetry_.bytes_per_sec_down = static_cast<float>(rate_window_bytes_received_) / elapsed_seconds;
    telemetry_.packets_per_sec_up = static_cast<float>(rate_window_packets_sent_) / elapsed_seconds;
    telemetry_.packets_per_sec_down = static_cast<float>(rate_window_packets_received_) / elapsed_seconds;
    telemetry_.reconciliation_corrections_per_sec =
        static_cast<float>(rate_window_reconciliation_corrections_) / elapsed_seconds;

    rate_window_bytes_sent_ = 0;
    rate_window_bytes_received_ = 0;
    rate_window_packets_sent_ = 0;
    rate_window_packets_received_ = 0;
    rate_window_reconciliation_corrections_ = 0;
    last_rate_update_ms_ = now;
}
